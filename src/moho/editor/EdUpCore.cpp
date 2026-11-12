// EDITOR: the first "up" natives (EdUpCommon.h): the library link check and the job pattern's own test.
//
//   ed_up_version()                -> { api = n, library = "faf_editor_logic", probe = true }   proof that upstream's
//   editor logic (faf_editor_logic) is linked and answers:
//                                     `probe` is the result of a call into it (the JSON quoting of
//                                     faf::editor::JsonQuoted). `api` counts the changes of the up natives.
//   ed_up_test_job_start(ms)       -> job        a worker that waits `ms` milliseconds (in 5 ms steps, reporting
//   progress, stopping when cancelled) and returns `ms` ed_up_test_job_poll(job)       -> { done, progress[, value |
//   cancelled] } | nil, message ed_up_test_job_cancel(job) / ed_up_test_job_free(job)
//
// The test job is what selftests of the polling pattern use; the areas that follow (vault, package, ...) copy its three
// natives.

#include <chrono>
#include <string>
#include <thread>

#include "editor/doc/EdJson.h"
#include "moho/editor/EdUpCommon.h"

namespace
{
  namespace up = moho::editor::up;

  constexpr int kUpApi = 1;

  int __cdecl cfunc_UpVersion(
    lua_State* const L
  )
  {
    lua_newtable(L);
    up::SetNumber(L, "api", kUpApi);
    up::SetString(L, "library", "faf_editor_logic");
    // A call into the library: if it is not linked this unit does not link either.
    up::SetBool(L, "probe", faf::editor::JsonQuoted("a\"b") == "\"a\\\"b\"");
    return 1;
  }

  struct TestResult
  {
    int value = 0;
    bool cancelled = false;
  };

  up::JobSet<TestResult>& TestJobs()
  {
    static up::JobSet<TestResult> jobs;
    return jobs;
  }

  int __cdecl cfunc_TestJobStart(
    lua_State* const L
  )
  {
    const int ms = static_cast<int>(luaL_checknumber(L, 1));
    const int id = TestJobs().Start([ms](up::JobContext& job) {
      TestResult result;
      for (int t = 0; t < ms; t += 5) {
        if (job.Cancelled()) {
          result.cancelled = true;
          return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        job.Report(ms > 0 ? static_cast<float>(t) / static_cast<float>(ms) : 1.0f);
      }
      result.value = ms;
      return result;
    });
    lua_pushnumber(L, id);
    return 1;
  }

  int __cdecl cfunc_TestJobPoll(
    lua_State* const L
  )
  {
    const int id = static_cast<int>(luaL_checknumber(L, 1));
    const up::JobStatus status = TestJobs().Poll(id);
    if (!status.known) {
      return up::Fail(L, "no such job");
    }
    up::PushJobStatus(L, status);
    if (status.done) {
      if (const auto result = TestJobs().Get(id)) {
        if (result->cancelled) {
          up::SetBool(L, "cancelled", true);
        } else {
          up::SetNumber(L, "value", result->value);
        }
      }
    }
    return 1;
  }

  int __cdecl cfunc_TestJobCancel(
    lua_State* const L
  )
  {
    TestJobs().Cancel(static_cast<int>(luaL_checknumber(L, 1)));
    return 0;
  }

  int __cdecl cfunc_TestJobFree(
    lua_State* const L
  )
  {
    TestJobs().Free(static_cast<int>(luaL_checknumber(L, 1)));
    return 0;
  }

  const up::Natives gEdUpCoreNatives{{
    {"ed_up_version", &cfunc_UpVersion, "ed_up_version() -> { api, library, probe }"},
    {"ed_up_test_job_start", &cfunc_TestJobStart, "ed_up_test_job_start(ms) -> job"},
    {"ed_up_test_job_poll",
     &cfunc_TestJobPoll,
     "ed_up_test_job_poll(job) -> { done, progress, value | cancelled } | nil, message"},
    {"ed_up_test_job_cancel", &cfunc_TestJobCancel, "ed_up_test_job_cancel(job)"},
    {"ed_up_test_job_free", &cfunc_TestJobFree, "ed_up_test_job_free(job)"},
  }};
} // namespace
