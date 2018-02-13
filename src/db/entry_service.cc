#include "db/entry_service.h"
#include <iostream>
#include "db/cruzdb.pb.h"

namespace cruzdb {

EntryService::EntryService(zlog::Log *log) :
  log_(log),
  stop_(false)
{
}

void EntryService::Start(uint64_t pos)
{
  pos_ = pos;
  intention_reader_ = std::thread(&EntryService::IntentionReader, this);
  io_thread_ = std::thread(&EntryService::IOEntry, this);
}

void EntryService::Stop()
{
  {
    std::lock_guard<std::mutex> l(lock_);
    stop_ = true;
  }

  ai_matcher.shutdown();

  for (auto& q : intention_queues_) {
    q->Stop();
  }

  intention_reader_.join();
  io_thread_.join();
}

void EntryCache::Insert(std::unique_ptr<Intention> intention)
{
  auto pos = intention->Position();
  std::lock_guard<std::mutex> lk(lock_);
  if (intentions_.size() > 10) {
    intentions_.erase(intentions_.begin());
  }
  intentions_.emplace(pos, std::move(intention));
}

// obvs this is silly to return a copy. the cache should be storing shared
// pointers or something like this.
boost::optional<Intention> EntryCache::FindIntention(uint64_t pos)
{
  std::lock_guard<std::mutex> lk(lock_);
  auto it = intentions_.find(pos);
  if (it != intentions_.end()) {
    return *(it->second);
  }
  return boost::none;
}

int EntryService::AppendIntention(std::unique_ptr<Intention> intention,
    uint64_t *pos)
{
  const auto blob = intention->Serialize();
  int ret = log_->Append(blob, pos);
  if (ret == 0) {
    intention->SetPosition(*pos);
    cache_.Insert(std::move(intention));
  }
  return ret;
}

void EntryService::IntentionReader()
{
  uint64_t pos;
  boost::optional<uint64_t> last_min_pos;

  while (true) {
    std::unique_lock<std::mutex> lk(lock_);

    if (stop_)
      return;

    if (intention_queues_.empty()) {
      last_min_pos = boost::none;
      continue;
    }

    // min pos requested by any queue
    auto min_pos = intention_queues_.front()->Position();
    for (auto& q : intention_queues_) {
      min_pos = std::min(min_pos, q->Position());
    }

    lk.unlock();

    if (!last_min_pos) {
      last_min_pos = min_pos;
      pos = min_pos;
    }

    if (min_pos < *last_min_pos) {
      last_min_pos = boost::none;
      continue;
    }

    last_min_pos = min_pos;

    // the cache may know that this pos is not an intention, and that additional
    // slots in the log can be skipped over...
    auto intention = cache_.FindIntention(pos);
    if (intention) {
      lk.lock();
      for (auto& q : intention_queues_) {
        if (pos >= q->Position()) {
          q->Push(*intention);
        }
      }
      lk.unlock();

      pos++;
      continue;
    }

    // obvs this should be populating the cache too..
    std::string data;
    int ret = log_->Read(pos, &data);
    if (ret) {
      if (ret == -ENOENT) {
        continue;
      }
      assert(0);
    }

    cruzdb_proto::LogEntry entry;
    assert(entry.ParseFromString(data));
    assert(entry.IsInitialized());

    switch (entry.type()) {
      case cruzdb_proto::LogEntry::INTENTION:
        lk.lock();
        for (auto& q : intention_queues_) {
          if (pos >= q->Position()) {
            q->Push(Intention(entry.intention(), pos));
          }
        }
        lk.unlock();
        break;

      case cruzdb_proto::LogEntry::AFTER_IMAGE:
        break;

      default:
        assert(0);
        exit(1);
    }

    pos++;
  }
}

void EntryService::IOEntry()
{
  uint64_t next = pos_;

  while (true) {
    {
      std::lock_guard<std::mutex> lk(lock_);
      if (stop_)
        break;
    }
    auto tail = CheckTail();
    assert(next <= tail);
    if (next == tail) {
      std::this_thread::sleep_for(std::chrono::microseconds(1000));
      continue;
    }
    while (next < tail) {
      std::unique_lock<std::mutex> lk(lock_);
      auto it = entry_cache_.find(next);
      if (it == entry_cache_.end()) {
        lk.unlock();
        std::string data;
        int ret = log_->Read(next, &data);
        if (ret) {
          if (ret == -ENOENT) {
            // we aren't going to spin on the tail, but we haven't yet implemented
            // a fill policy, and really we shouldn't have holes in our
            // single-node setup, so we just spin on the hole for now...
            continue;
          }
          assert(0);
        }

        cruzdb_proto::LogEntry entry;
        assert(entry.ParseFromString(data));
        assert(entry.IsInitialized());

        CacheEntry cache_entry;

        switch (entry.type()) {
          case cruzdb_proto::LogEntry::AFTER_IMAGE:
            cache_entry.type = CacheEntry::EntryType::AFTERIMAGE;
            cache_entry.after_image =
              std::make_shared<cruzdb_proto::AfterImage>(
                  std::move(entry.after_image()));
            ai_matcher.push(entry.after_image(), next);
            break;

          case cruzdb_proto::LogEntry::INTENTION:
            cache_entry.type = CacheEntry::EntryType::INTENTION;
            cache_entry.intention = std::make_shared<Intention>(
                entry.intention(), next);
            break;

          default:
            assert(0);
            exit(1);
        }

        lk.lock();
        entry_cache_.emplace(next, cache_entry);
        lk.unlock();
      }

      next++;
    }
  }
}

EntryService::IntentionQueue *EntryService::NewIntentionQueue(uint64_t pos)
{
  auto queue = std::make_unique<IntentionQueue>(pos);
  auto ret = queue.get();
  std::lock_guard<std::mutex> lk(lock_);
  intention_queues_.emplace_back(std::move(queue));
  return ret;
}

EntryService::IntentionQueue::IntentionQueue(uint64_t pos) :
  pos_(pos),
  stop_(false)
{
}

void EntryService::IntentionQueue::Stop()
{
  {
    std::lock_guard<std::mutex> lk(lock_);
    stop_ = true;
  }
  cond_.notify_all();
}

boost::optional<Intention> EntryService::IntentionQueue::Wait()
{
  std::unique_lock<std::mutex> lk(lock_);
  cond_.wait(lk, [this] { return !q_.empty() || stop_; });
  if (stop_) {
    return boost::none;
  }
  assert(!q_.empty());
  auto i = q_.front();
  q_.pop();
  return i;
}

uint64_t EntryService::IntentionQueue::Position() const
{
  std::lock_guard<std::mutex> lk(lock_);
  return pos_;
}

void EntryService::IntentionQueue::Push(Intention intention)
{
  std::lock_guard<std::mutex> lk(lock_);
  assert(pos_ <= intention.Position());
  pos_ = intention.Position() + 1;
  q_.emplace(intention);
  cond_.notify_one();
}

std::list<std::shared_ptr<Intention>>
EntryService::ReadIntentions(std::vector<uint64_t> addrs)
{
  assert(!addrs.empty());
  std::list<std::shared_ptr<Intention>> intentions;
  std::vector<uint64_t> missing;

  std::unique_lock<std::mutex> lk(lock_);
  for (auto pos : addrs) {
    auto it = entry_cache_.find(pos);
    if (it != entry_cache_.end()) {
      assert(it->second.type == CacheEntry::EntryType::INTENTION);
      intentions.push_back(it->second.intention);
    } else {
      missing.push_back(pos);
    }
  }

  lk.unlock();
  for (auto pos : missing) {
    // TODO: dump positions into an i/o queue...
    std::string data;
    int ret = log_->Read(pos, &data);
    assert(ret == 0);

    cruzdb_proto::LogEntry entry;
    assert(entry.ParseFromString(data));
    assert(entry.IsInitialized());
    assert(entry.type() == cruzdb_proto::LogEntry::INTENTION);

    // this is rather inefficient. below perhaps choose
    // insert/insert_or_assign, etc... c++17
    auto intention = std::make_shared<Intention>(
        entry.intention(), pos);

    CacheEntry cache_entry;
    cache_entry.type = CacheEntry::EntryType::INTENTION;
    cache_entry.intention = intention;

    lk.lock();
    auto p = entry_cache_.emplace(pos, cache_entry);
    lk.unlock();

    intentions.emplace_back(p.first->second.intention);
  }

  return intentions;
}

EntryService::PrimaryAfterImageMatcher::PrimaryAfterImageMatcher() :
  shutdown_(false),
  matched_watermark_(0)
{
}

void EntryService::PrimaryAfterImageMatcher::watch(
    std::vector<SharedNodeRef> delta,
    std::unique_ptr<PersistentTree> intention)
{
  std::lock_guard<std::mutex> lk(lock_);

  const auto ipos = intention->Intention();

  auto it = afterimages_.find(ipos);
  if (it == afterimages_.end()) {
    afterimages_.emplace(ipos,
        PrimaryAfterImage{boost::none,
        std::move(intention),
        std::move(delta)});
  } else {
    assert(it->second.pos);
    assert(!it->second.tree);
    intention->SetAfterImage(*it->second.pos);
    it->second.pos = boost::none;
    matched_.emplace_back(std::make_pair(std::move(delta),
        std::move(intention)));
    cond_.notify_one();
  }

  gc();
}

void EntryService::PrimaryAfterImageMatcher::push(
    const cruzdb_proto::AfterImage& ai, uint64_t pos)
{
  std::lock_guard<std::mutex> lk(lock_);

  const auto ipos = ai.intention();
  if (ipos <= matched_watermark_) {
    return;
  }

  auto it = afterimages_.find(ipos);
  if (it == afterimages_.end()) {
    afterimages_.emplace(ipos, PrimaryAfterImage{pos, nullptr, {}});
  } else if (!it->second.pos && it->second.tree) {
    assert(it->second.tree->Intention() == ipos);
    it->second.tree->SetAfterImage(pos);
    matched_.emplace_back(std::make_pair(std::move(it->second.delta),
        std::move(it->second.tree)));
    cond_.notify_one();
  }

  gc();
}

std::pair<std::vector<SharedNodeRef>,
  std::unique_ptr<PersistentTree>>
EntryService::PrimaryAfterImageMatcher::match()
{
  std::unique_lock<std::mutex> lk(lock_);

  cond_.wait(lk, [&] { return !matched_.empty() || shutdown_; });

  if (shutdown_) {
    return std::make_pair(std::vector<SharedNodeRef>(), nullptr);
  }

  assert(!matched_.empty());

  auto tree = std::move(matched_.front());
  matched_.pop_front();

  return std::move(tree);
}

void EntryService::PrimaryAfterImageMatcher::shutdown()
{
  std::lock_guard<std::mutex> l(lock_);
  shutdown_ = true;
  cond_.notify_one();
}

void EntryService::PrimaryAfterImageMatcher::gc()
{
  auto it = afterimages_.begin();
  while (it != afterimages_.end()) {
    auto ipos = it->first;
    assert(matched_watermark_ < ipos);
    auto& pai = it->second;
    if (!pai.pos && !pai.tree) {
      matched_watermark_ = ipos;
      it = afterimages_.erase(it);
    } else {
      // as long as the watermark is positioned such that no unmatched intention
      // less than the water is in the index, then gc could move forward and
      // continue removing matched entries.
      break;
    }
  }
}

uint64_t EntryService::CheckTail()
{
  uint64_t pos;
  int ret = log_->CheckTail(&pos);
  if (ret) {
    std::cerr << "failed to check tail" << std::endl;
    assert(0);
    exit(1);
  }
  return pos;
}

uint64_t EntryService::Append(cruzdb_proto::Intention& intention)
{
  cruzdb_proto::LogEntry entry;
  entry.set_type(cruzdb_proto::LogEntry::INTENTION);
  entry.set_allocated_intention(&intention);
  assert(entry.IsInitialized());

  std::string blob;
  assert(entry.SerializeToString(&blob));
  entry.release_intention();

  uint64_t pos;
  int ret = log_->Append(blob, &pos);
  if (ret) {
    std::cerr << "failed to append intention" << std::endl;
    assert(0);
    exit(1);
  }
  return pos;
}

uint64_t EntryService::Append(cruzdb_proto::AfterImage& after_image)
{
  cruzdb_proto::LogEntry entry;
  entry.set_type(cruzdb_proto::LogEntry::AFTER_IMAGE);
  entry.set_allocated_after_image(&after_image);
  assert(entry.IsInitialized());

  std::string blob;
  assert(entry.SerializeToString(&blob));
  entry.release_after_image();

  uint64_t pos;
  int ret = log_->Append(blob, &pos);
  if (ret) {
    std::cerr << "failed to append after image" << std::endl;
    assert(0);
    exit(1);
  }
  return pos;
}

}
