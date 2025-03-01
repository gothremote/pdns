/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "rec-taskqueue.hh"
#include "taskqueue.hh"
#include "lock.hh"
#include "logging.hh"
#include "stat_t.hh"
#include "syncres.hh"

// For rate limiting purposes we maintain a set of tasks recently submitted.
class TimedSet
{
public:
  TimedSet(time_t t) :
    d_expiry_seconds(t)
  {
  }

  uint64_t purge(time_t now)
  {
    // This purge is relatively cheap, as we're walking an ordered index
    uint64_t erased = 0;
    auto& ind = d_set.template get<time_t>();
    auto it = ind.begin();
    while (it != ind.end()) {
      if (it->d_ttd < now) {
        ++erased;
        it = ind.erase(it);
      }
      else {
        break;
      }
    }
    return erased;
  }

  bool insert(time_t now, const pdns::ResolveTask& task)
  {
    // We periodically purge
    if (++d_count % 1024 == 0) {
      purge(now);
    }
    time_t ttd = now + d_expiry_seconds;
    bool inserted = d_set.emplace(task, ttd).second;
    if (!inserted) {
      uint64_t erased = purge(now);
      // Try again if the purge deleted at least one entry
      if (erased > 0) {
        inserted = d_set.emplace(task, ttd).second;
      }
    }
    return inserted;
  }

  void clear()
  {
    d_set.clear();
  }

private:
  struct Entry
  {
    Entry(const pdns::ResolveTask& task, time_t ttd) :
      d_task(task), d_ttd(ttd) {}
    pdns::ResolveTask d_task;
    time_t d_ttd;
  };

  typedef multi_index_container<Entry,
                                indexed_by<
                                  ordered_unique<tag<pdns::ResolveTask>, member<Entry, pdns::ResolveTask, &Entry::d_task>>,
                                  ordered_non_unique<tag<time_t>, member<Entry, time_t, &Entry::d_ttd>>>>
    timed_set_t;
  timed_set_t d_set;
  time_t d_expiry_seconds;
  unsigned int d_count{0};
};

struct Queue
{
  pdns::TaskQueue queue;
  TimedSet rateLimitSet{60};
};
static LockGuarded<Queue> s_taskQueue;

struct taskstats
{
  pdns::stat_t pushed;
  pdns::stat_t run;
  pdns::stat_t exceptions;
};

static struct taskstats s_almost_expired_tasks;
static struct taskstats s_resolve_tasks;

static void resolve(const struct timeval& now, bool logErrors, const pdns::ResolveTask& task) noexcept
{
  auto log = g_slog->withName("taskq")->withValues("name", Logging::Loggable(task.d_qname), "qtype", Logging::Loggable(QType(task.d_qtype).toString()));
  const string msg = "Exception while running a background ResolveTask";
  SyncRes sr(now);
  vector<DNSRecord> ret;
  sr.setRefreshAlmostExpired(task.d_refreshMode);
  bool ex = true;
  try {
    log->info(Logr::Debug, "resolving");
    int res = sr.beginResolve(task.d_qname, QType(task.d_qtype), QClass::IN, ret);
    ex = false;
    log->info(Logr::Debug, "done", "rcode", Logging::Loggable(res), "records", Logging::Loggable(ret.size()));
  }
  catch (const std::exception& e) {
    log->error(Logr::Error, msg, e.what());
  }
  catch (const PDNSException& e) {
    log->error(Logr::Error, msg, e.reason);
  }
  catch (const ImmediateServFailException& e) {
    if (logErrors) {
      log->error(Logr::Error, msg, e.reason);
    }
  }
  catch (const PolicyHitException& e) {
    if (logErrors) {
      log->error(Logr::Notice, msg, "PolicyHit");
    }
  }
  catch (...) {
    log->error(Logr::Error, msg, "Unexpectec exception");
  }
  if (ex) {
    if (task.d_refreshMode) {
      ++s_almost_expired_tasks.exceptions;
    }
    else {
      ++s_resolve_tasks.exceptions;
    }
  }
  else {
    if (task.d_refreshMode) {
      ++s_almost_expired_tasks.run;
    }
    else {
      ++s_resolve_tasks.run;
    }
  }
}

void runTaskOnce(bool logErrors)
{
  pdns::ResolveTask task;
  {
    auto lock = s_taskQueue.lock();
    if (lock->queue.empty()) {
      return;
    }
    task = lock->queue.pop();
  }
  bool expired = task.run(logErrors);
  if (expired) {
    s_taskQueue.lock()->queue.incExpired();
  }
}

void pushAlmostExpiredTask(const DNSName& qname, uint16_t qtype, time_t deadline)
{
  if (SyncRes::isUnsupported(qtype)) {
    auto log = g_slog->withName("taskq")->withValues("name", Logging::Loggable(qname), "qtype", Logging::Loggable(QType(qtype).toString()));
    log->error(Logr::Error, "Cannot push task", "qtype unsupported");
    return;
  }
  pdns::ResolveTask task{qname, qtype, deadline, true, resolve};
  s_taskQueue.lock()->queue.push(std::move(task));
  ++s_almost_expired_tasks.pushed;
}

void pushResolveTask(const DNSName& qname, uint16_t qtype, time_t now, time_t deadline)
{
  if (SyncRes::isUnsupported(qtype)) {
    auto log = g_slog->withName("taskq")->withValues("name", Logging::Loggable(qname), "qtype", Logging::Loggable(QType(qtype).toString()));
    log->error(Logr::Error, "Cannot push task", "qtype unsupported");
    return;
  }
  pdns::ResolveTask task{qname, qtype, deadline, false, resolve};
  auto lock = s_taskQueue.lock();
  bool inserted = lock->rateLimitSet.insert(now, task);
  if (inserted) {
    lock->queue.push(std::move(task));
    ++s_resolve_tasks.pushed;
  }
}

uint64_t getTaskPushes()
{
  return s_taskQueue.lock()->queue.getPushes();
}

uint64_t getTaskExpired()
{
  return s_taskQueue.lock()->queue.getExpired();
}

uint64_t getTaskSize()
{
  return s_taskQueue.lock()->queue.size();
}

void taskQueueClear()
{
  auto lock = s_taskQueue.lock();
  lock->queue.clear();
  lock->rateLimitSet.clear();
}

pdns::ResolveTask taskQueuePop()
{
  return s_taskQueue.lock()->queue.pop();
}

uint64_t getAlmostExpiredTasksPushed()
{
  return s_almost_expired_tasks.pushed;
}

uint64_t getAlmostExpiredTasksRun()
{
  return s_almost_expired_tasks.run;
}

uint64_t getAlmostExpiredTaskExceptions()
{
  return s_almost_expired_tasks.exceptions;
}

uint64_t getResolveTasksPushed()
{
  return s_almost_expired_tasks.pushed;
}

uint64_t getResolveTasksRun()
{
  return s_almost_expired_tasks.run;
}

uint64_t getResolveTaskExceptions()
{
  return s_almost_expired_tasks.exceptions;
}
