// EDITOR: the binding scaffold of the "up" natives (docs/projects/editor/EDITOR_PORT_ANALYSIS.md, work package WP1).
//
// "Up" is upstream's editor logic (src/editor/{doc,tools,graph,undo}, built as faf_editor_logic, namespace faf::editor): text in, text out, no engine objects.
// Each area of it is bound to Lua by one translation unit of moho/editor named EdUp<Area>.cpp (EdUpCore.cpp holds ed_up_version); this header is what those
// units share, so each stays a thin layer over the library:
//
//   * registration:   a unit declares `static const moho::editor::up::Natives bootstrap{{ {"ed_xxx", &cfunc_Xxx, "ed_xxx(a) -> b"}, ... }};` and the natives are in the
//                     UI Lua state (the "User" init set, as ed_field_* and the other ed_* natives are).
//   * tables:         plain Lua tables in and out; the helpers read a field with a default (a missing field is not an error, the library's own defaults apply) and
//                     push lists, so a binding is "table -> struct, call, struct -> table" and nothing else. Failures are `nil, message` (Fail), never a Lua error,
//                     except a wrong argument type (Arg* helpers, luaL_error as the engine's natives do).
//   * jobs:           anything that waits (HTTP, a zip of a map folder, a vault download) runs on a worker thread of the library and is POLLED from Lua:
//                       ed_xxx_start(args...)  -> job id        (the job runs on its own thread; the UI thread never blocks)
//                       ed_xxx_poll(job)       -> { done = bool, progress = 0..1, ... }, and once `done`, the result fields or `error`
//                       ed_xxx_cancel(job)     -> nothing       (the worker is asked to stop; the job still ends with `done`)
//                       ed_xxx_free(job)       -> nothing       (forgets the result; a running job is cancelled and joined first)
//                     `JobSet<Result>` below is the registry: Start() takes a callable `Result(JobContext&)`, Poll() / Take() are what the natives call. The registry
//                     is touched by the Lua thread only; the worker sees its JobContext (cancel flag, progress) and nothing else, so the library needs no locks.
//
// Everything here is header-only and includes no engine header but the Lua C API, so a unit that binds a library area stays small.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"

namespace moho::editor::up
{
  // ---- registration ---------------------------------------------------------------------------------------------------------------------------

  struct Native
  {
    const char* name;
    int(__cdecl* function)(lua_State*);
    const char* doc;
  };

  /** The UI Lua state's init set (where the editor's natives live). */
  [[nodiscard]] inline moho::CScrLuaInitFormSet& UserLuaInitSet()
  {
    if (moho::CScrLuaInitFormSet* const set = moho::SCR_FindLuaInitFormSet("User"); set != nullptr) {
      return *set;
    }
    static moho::CScrLuaInitFormSet fallbackSet("User");
    return fallbackSet;
  }

  /** A static object of a binding unit: registers its natives when the executable starts, as the other editor natives do. */
  struct Natives
  {
    Natives(std::initializer_list<Native> list)
    {
      moho::CScrLuaInitFormSet& set = UserLuaInitSet();
      for (const Native& n : list) {
        (void)new moho::CScrLuaBinder(set, n.name, n.function, nullptr, "<global>", n.doc);
      }
    }
  };

  // ---- results --------------------------------------------------------------------------------------------------------------------------------

  /** `nil, message` for a Lua caller; returns the number of results. */
  inline int Fail(lua_State* const L, const std::string& message)
  {
    lua_pushnil(L);
    lua_pushstring(L, message.c_str());
    return 2;
  }

  // ---- reading a table argument ---------------------------------------------------------------------------------------------------------------------
  //
  // `index` is the table's stack index (absolute or relative to the top of the stack at call time); a non-table argument reads as an empty table.

  /** Pushes t[key] (nil when `index` is no table) and returns its type. Pop it with lua_settop(L, -2). */
  inline int PushField(lua_State* const L, const int index, const char* const key)
  {
    const int abs = index < 0 && index > LUA_REGISTRYINDEX ? lua_gettop(L) + index + 1 : index;
    if (lua_type(L, abs) != LUA_TTABLE) {
      lua_pushnil(L);
      return LUA_TNIL;
    }
    lua_pushstring(L, key);
    lua_gettable(L, abs);
    return lua_type(L, -1);
  }

  [[nodiscard]] inline std::string FieldString(lua_State* const L, const int index, const char* const key, const std::string& fallback = std::string())
  {
    std::string out = fallback;
    if (PushField(L, index, key) == LUA_TSTRING) {
      out.assign(lua_tostring(L, -1), lua_strlen(L, -1));
    }
    lua_settop(L, -2);
    return out;
  }

  [[nodiscard]] inline double FieldNumber(lua_State* const L, const int index, const char* const key, const double fallback = 0.0)
  {
    double out = fallback;
    if (PushField(L, index, key) == LUA_TNUMBER) {
      out = static_cast<double>(lua_tonumber(L, -1));
    }
    lua_settop(L, -2);
    return out;
  }

  [[nodiscard]] inline int FieldInt(lua_State* const L, const int index, const char* const key, const int fallback = 0)
  {
    return static_cast<int>(FieldNumber(L, index, key, static_cast<double>(fallback)));
  }

  [[nodiscard]] inline bool FieldBool(lua_State* const L, const int index, const char* const key, const bool fallback = false)
  {
    bool out = fallback;
    if (PushField(L, index, key) == LUA_TBOOLEAN) {
      out = lua_toboolean(L, -1) != 0;
    }
    lua_settop(L, -2);
    return out;
  }

  /** The strings of the array t[key] (non-string entries are skipped). */
  [[nodiscard]] inline std::vector<std::string> FieldStringList(lua_State* const L, const int index, const char* const key)
  {
    std::vector<std::string> out;
    if (PushField(L, index, key) == LUA_TTABLE) {
      const int list = lua_gettop(L);
      for (int i = 1;; ++i) {
        lua_rawgeti(L, list, i);
        const int type = lua_type(L, -1);
        if (type == LUA_TNIL) {
          lua_settop(L, -2);
          break;
        }
        if (type == LUA_TSTRING) {
          out.emplace_back(lua_tostring(L, -1), lua_strlen(L, -1));
        }
        lua_settop(L, -2);
      }
    }
    lua_settop(L, -2);
    return out;
  }

  /** The numbers of the array t[key] (non-number entries are skipped). */
  [[nodiscard]] inline std::vector<double> FieldNumberList(lua_State* const L, const int index, const char* const key)
  {
    std::vector<double> out;
    if (PushField(L, index, key) == LUA_TTABLE) {
      const int list = lua_gettop(L);
      for (int i = 1;; ++i) {
        lua_rawgeti(L, list, i);
        const int type = lua_type(L, -1);
        if (type == LUA_TNIL) {
          lua_settop(L, -2);
          break;
        }
        if (type == LUA_TNUMBER) {
          out.push_back(static_cast<double>(lua_tonumber(L, -1)));
        }
        lua_settop(L, -2);
      }
    }
    lua_settop(L, -2);
    return out;
  }

  /** A string argument (a Lua error naming `what` when it is not one). */
  [[nodiscard]] inline std::string ArgString(lua_State* const L, const int index, const char* const what)
  {
    if (lua_type(L, index) != LUA_TSTRING) {
      luaL_error(L, "%s: argument %d must be a string", what, index);
    }
    return std::string(lua_tostring(L, index), lua_strlen(L, index));
  }

  // ---- writing a table result -----------------------------------------------------------------------------------------------------------------------
  //
  // The table is on top of the stack (lua_newtable first); each call adds one field and leaves the stack as it was.

  inline void SetString(lua_State* const L, const char* const key, const std::string& value)
  {
    lua_pushstring(L, key);
    lua_pushlstring(L, value.data(), value.size());
    lua_settable(L, -3);
  }

  inline void SetNumber(lua_State* const L, const char* const key, const double value)
  {
    lua_pushstring(L, key);
    lua_pushnumber(L, static_cast<lua_Number>(value));
    lua_settable(L, -3);
  }

  inline void SetBool(lua_State* const L, const char* const key, const bool value)
  {
    lua_pushstring(L, key);
    lua_pushboolean(L, value ? 1 : 0);
    lua_settable(L, -3);
  }

  inline void SetStringList(lua_State* const L, const char* const key, const std::vector<std::string>& values)
  {
    lua_pushstring(L, key);
    lua_newtable(L);
    for (std::size_t i = 0; i < values.size(); ++i) {
      lua_pushlstring(L, values[i].data(), values[i].size());
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
  }

  inline void SetNumberList(lua_State* const L, const char* const key, const std::vector<double>& values)
  {
    lua_pushstring(L, key);
    lua_newtable(L);
    for (std::size_t i = 0; i < values.size(); ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(values[i]));
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
  }

  // ---- jobs (polled from Lua) -----------------------------------------------------------------------------------------------------------------------

  /** What a worker sees of its job: whether it should stop, and where it reports progress (0..1). */
  struct JobContext
  {
    std::atomic<bool> cancel{false};
    std::atomic<float> progress{0.0f};
    [[nodiscard]] bool Cancelled() const noexcept { return cancel.load(std::memory_order_relaxed); }
    void Report(const float value) noexcept { progress.store(value, std::memory_order_relaxed); }
  };

  /** What Poll() says of a job. */
  struct JobStatus
  {
    bool known = false; ///< the id names a job of this set
    bool done = false;
    float progress = 0.0f;
  };

  /** A set of jobs whose workers return a `Result`; used from the Lua thread only. Ids are never reused. */
  template <class Result>
  class JobSet
  {
  public:
    using Work = std::function<Result(JobContext&)>;

    JobSet() = default;
    JobSet(const JobSet&) = delete;
    JobSet& operator=(const JobSet&) = delete;
    ~JobSet() { Clear(); }

    /** Starts `work` on a new thread; returns its id. */
    int Start(Work work)
    {
      const int id = nextId_++;
      auto job = std::make_shared<Job>();
      job->thread = std::thread([job, work = std::move(work)]() {
        Result value = work(job->context);
        {
          const std::lock_guard<std::mutex> lock(job->mutex);
          job->result = std::move(value);
        }
        job->context.Report(1.0f);
        job->done.store(true, std::memory_order_release);
      });
      jobs_[id] = std::move(job);
      return id;
    }

    [[nodiscard]] JobStatus Poll(const int id) const
    {
      JobStatus status;
      const auto it = jobs_.find(id);
      if (it == jobs_.end()) {
        return status;
      }
      status.known = true;
      status.done = it->second->done.load(std::memory_order_acquire);
      status.progress = it->second->context.progress.load(std::memory_order_relaxed);
      return status;
    }

    /** The result of a finished job (a copy; the job stays until Free()); empty while it runs or for an unknown id. */
    [[nodiscard]] std::optional<Result> Get(const int id) const
    {
      const auto it = jobs_.find(id);
      if (it == jobs_.end() || !it->second->done.load(std::memory_order_acquire)) {
        return std::nullopt;
      }
      const std::lock_guard<std::mutex> lock(it->second->mutex);
      return it->second->result;
    }

    /** Asks the worker to stop (it decides when; the job still ends as done). */
    void Cancel(const int id) const
    {
      const auto it = jobs_.find(id);
      if (it != jobs_.end()) {
        it->second->context.cancel.store(true, std::memory_order_relaxed);
      }
    }

    /** Forgets a job: cancels and joins it when it still runs. */
    void Free(const int id)
    {
      const auto it = jobs_.find(id);
      if (it == jobs_.end()) {
        return;
      }
      Retire(*it->second);
      jobs_.erase(it);
    }

    /** Frees every job (at exit and on a map change). */
    void Clear()
    {
      for (auto& entry : jobs_) {
        Retire(*entry.second);
      }
      jobs_.clear();
    }

    [[nodiscard]] std::size_t Count() const noexcept { return jobs_.size(); }

  private:
    struct Job
    {
      JobContext context;
      std::atomic<bool> done{false};
      std::mutex mutex;
      std::optional<Result> result;
      std::thread thread;
    };

    static void Retire(Job& job)
    {
      job.context.cancel.store(true, std::memory_order_relaxed);
      if (job.thread.joinable()) {
        job.thread.join();
      }
    }

    std::map<int, std::shared_ptr<Job>> jobs_;
    int nextId_ = 1;
  };

  /** Starts the poll table of a job: { done = bool, progress = n } on top of the stack (the caller adds the result fields once `done`). */
  inline void PushJobStatus(lua_State* const L, const JobStatus& status)
  {
    lua_newtable(L);
    SetBool(L, "done", status.done);
    SetNumber(L, "progress", status.progress);
  }
} // namespace moho::editor::up
