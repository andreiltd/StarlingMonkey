#include "extension-api.h"
#include "allocator.h"
#include "debugger.h"
#include "encode.h"
#include "event_loop.h"
#include "script_loader.h"

// TODO: remove these once the warnings are fixed
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#pragma clang diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#include "js/CompilationAndEvaluation.h"
#include "js/Modules.h"
#include "js/ForOfIterator.h"
#include "js/Initialization.h"
#include "js/Promise.h"
#include "jsfriendapi.h"
#pragma clang diagnostic pop

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>

#ifdef MEM_STATS
#include <string>
#include "memory-reporting.h"

size_t size_of_cb(const void *ptr) { return ptr ? sizeof(ptr) : 0; }

static bool dump_mem_stats(JSContext *cx) {
  SimpleJSRuntimeStats rtStats(&size_of_cb);
  if (!JS::CollectRuntimeStats(cx, &rtStats, nullptr, false))
    return false;
  std::string rtPath = "rt";
  size_t rtTotal;
  ReportJSRuntimeExplicitTreeStats(rtStats, rtPath, nullptr, false, &rtTotal);

  printf("compartment counts: %zu sys, %zu usr\n", JS::SystemCompartmentCount(cx),
         JS::UserCompartmentCount(cx));
  printf("GC heap total: %zu\n",
         size_t(JS_GetGCParameter(cx, JSGC_TOTAL_CHUNKS)) * js::gc::ChunkSize);
  printf("GC heap unused: %zu\n",
         size_t(JS_GetGCParameter(cx, JSGC_UNUSED_CHUNKS)) * js::gc::ChunkSize);

  return true;
}
#endif // MEM_STATS

#ifndef DEBUG_LOGGING
#define DEBUG_LOGGING false
#endif

#define TRACE(...)                                                                                 \
  if (config_->verbose) {                                                                          \
    std::cout << "trace(" << __func__ << ":" << __LINE__ << "): " << __VA_ARGS__ << std::endl;     \
    fflush(stdout);                                                                                \
  }

using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::system_clock;

using JS::Value;

using api::Engine;
using api::EngineState;
using api::EngineConfig;

void dump_error(JSContext *cx, HandleValue error, bool *has_stack, FILE *fp);

__attribute__((weak))
bool debug_logging_enabled() { return DEBUG_LOGGING; }
#define LOG(...)                                                                                   \
  if (debug_logging_enabled()) {                                                                   \
    printf(__VA_ARGS__);                                                                           \
    fflush(stdout);                                                                                \
  }

JS::UniqueChars stringify_value(JSContext *cx, JS::HandleValue value) {
  JS::RootedString str(cx, JS_ValueToSource(cx, value));
  if (!str)
    return nullptr;

  return JS_EncodeStringToUTF8(cx, str);
}

bool dump_value(JSContext *cx, JS::Value val, FILE *fp) {
  RootedValue value(cx, val);
  JS::UniqueChars utf8chars = stringify_value(cx, value);
  if (!utf8chars) {
    return false;
  }

  fprintf(fp, "%s\n", utf8chars.get());
  return true;
}

bool print_stack(JSContext *cx, HandleObject stack, FILE *fp) {
  RootedString stackStr(cx);
  if (!BuildStackString(cx, nullptr, stack, &stackStr, 2)) {
    return false;
  }

  auto utf8chars = core::encode(cx, stackStr);
  if (!utf8chars) {
    return false;
  }

  fprintf(fp, "%s\n", utf8chars.begin());
  return true;
}

bool print_stack(JSContext *cx, FILE *fp) {
  RootedObject stackp(cx);
  if (!JS::CaptureCurrentStack(cx, &stackp))
    return false;
  return print_stack(cx, stackp, fp);
}

void print_cause(JSContext *cx, HandleValue error, FILE *fp) {
  bool has_cause = false;

  if (error.isObject()) {
    RootedObject err(cx, &error.toObject());
    if (!JS_HasProperty(cx, err, "cause", &has_cause)) {
      return;
    }

    if (has_cause) {
      JS::RootedValue cause_val(cx);
      if (!JS_GetProperty(cx, err, "cause", &cause_val)) {
        return;
      }

      fprintf(stderr, "Caused by: ");

      bool has_stack;
      dump_error(cx, cause_val, &has_stack, fp);
    }
  }
}

void dump_error(JSContext *cx, HandleValue error, bool *has_stack, FILE *fp) {
  bool reported = false;
  RootedObject stack(cx);

  if (error.isObject()) {
    RootedObject err(cx, &error.toObject());
    JSErrorReport *report = JS_ErrorFromException(cx, err);
    if (report) {
      fprintf(stderr, "%s\n", report->message().c_str());
      reported = true;
    }

    stack = JS::ExceptionStackOrNull(err);
  }

  // If the rejection reason isn't an `Error` object, we just dump the value
  // as-is.
  if (!reported) {
    dump_value(cx, error, stderr);
  }

  if (stack) {
    *has_stack = true;
    fprintf(stderr, "Stack:\n");
    print_stack(cx, stack, stderr);
  } else {
    *has_stack = false;
  }

  print_cause(cx, error, fp);
}

void dump_promise_rejection(JSContext *cx, HandleValue reason, HandleObject promise, FILE *fp) {
  bool has_stack;
  dump_error(cx, reason, &has_stack, fp);

  // If the rejection reason isn't an `Error` object, we can't get an exception
  // stack from it. In that case, fall back to getting the stack from the
  // promise resolution site. These should be identical in many cases, such as
  // for exceptions thrown in async functions, but for some reason the
  // resolution site stack seems to sometimes be wrong, so we only fall back to
  // it as a last resort.
  if (!has_stack) {
    RootedObject stack(cx, JS::GetPromiseResolutionSite(promise));
    fprintf(stderr, "Stack:\n");
    print_stack(cx, stack, stderr);
  }
}

/* The class of the global object. */
static JSClass global_class = {"global", JSCLASS_GLOBAL_FLAGS, &JS::DefaultGlobalClassOps};

JS::PersistentRootedObject GLOBAL;
JS::PersistentRootedObject INIT_SCRIPT_GLOBAL;
static ScriptLoader* scriptLoader;
JS::PersistentRootedObject unhandledRejectedPromises;

void gc_callback(JSContext *cx, JSGCStatus status, JS::GCReason reason, void *data) {
  LOG("gc for reason %s, %s\n", JS::ExplainGCReason(reason), status ? "end" : "start");
}

static void rejection_tracker(JSContext *cx, bool mutedErrors, JS::HandleObject promise,
                              JS::PromiseRejectionHandlingState state, void *data) {
  RootedValue promiseVal(cx, JS::ObjectValue(*promise));

  switch (state) {
  case JS::PromiseRejectionHandlingState::Unhandled: {
    if (!JS::SetAdd(cx, unhandledRejectedPromises, promiseVal)) {
      // Note: we unconditionally print these, since they almost always indicate
      // serious bugs.
      fprintf(stderr, "Adding an unhandled rejected promise to the promise "
                      "rejection tracker failed");
    }
    return;
  }
  case JS::PromiseRejectionHandlingState::Handled: {
    bool deleted = false;
    if (!JS::SetDelete(cx, unhandledRejectedPromises, promiseVal, &deleted)) {
      // Note: we unconditionally print these, since they almost always indicate
      // serious bugs.
      fprintf(stderr, "Removing an handled rejected promise from the promise "
                      "rejection tracker failed");
    }
  }
  }
}

bool math_random(JSContext *cx, unsigned argc, Value *vp) {
  auto res = host_api::Random::get_u32();
  MOZ_ASSERT(!res.is_err());
  double newvalue = static_cast<double>(res.unwrap()) / std::pow(2.0, 32.0);

  JS::CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setDouble(newvalue);
  return true;
}

bool fix_math_random(JSContext *cx, HandleObject global) {
  JS::RootedValue math_val(cx);
  if (!JS_GetProperty(cx, global, "Math", &math_val)) {
    return false;
  }
  JS::RootedObject math(cx, &math_val.toObject());

  const JSFunctionSpec funs[] = {JS_FN("random", math_random, 0, 0), JS_FS_END};
  return JS_DefineFunctions(cx, math, funs);
}

static Engine *ENGINE;
JS::PersistentRootedValue SCRIPT_VALUE;

bool create_content_global(JSContext * cx) {
  JS::RealmOptions options;
  options.creationOptions().setStreamsEnabled(true);

  JS::DisableIncrementalGC(cx);
  // JS_SetGCParameter(cx, JSGC_MAX_EMPTY_CHUNK_COUNT, 1);

  RootedObject global(
      cx, JS_NewGlobalObject(cx, &global_class, nullptr, JS::FireOnNewGlobalHook, options));
  if (!global) {
    return false;
  }

  JSAutoRealm ar(cx, global);
  if (!JS::InitRealmStandardClasses(cx) || !fix_math_random(cx, global)) {
    return false;
  }

  GLOBAL.init(cx, global);
  return true;
}

static bool define_builtin_module(JSContext *cx, unsigned argc, Value *vp);

bool create_initializer_global(Engine *engine) {
  auto cx = engine->cx();

  JS::RealmOptions options;
  options.creationOptions()
      .setStreamsEnabled(true)
      .setExistingCompartment(engine->global());

  static JSClass global_class = {"global", JSCLASS_GLOBAL_FLAGS, &JS::DefaultGlobalClassOps};
  RootedObject global(cx);
  global = JS_NewGlobalObject(cx, &global_class, nullptr, JS::DontFireOnNewGlobalHook, options);
  if (!global) {
    return false;
  }

  JSAutoRealm ar(cx, global);

  if (!JS_DefineFunction(cx, global, "defineBuiltinModule", ::define_builtin_module, 2, 0) ||
      !JS_DefineProperty(cx, global, "contentGlobal", ENGINE->global(), JSPROP_READONLY) ||
      !JS_DefineFunction(cx, global, "print", content_debugger::dbg_print, 1, 0)) {
    return false;
  }

  INIT_SCRIPT_GLOBAL.init(cx, global);
  return true;
}

bool init_js(const EngineConfig& config) {
  JS_Init();

  JSContext *cx = JS_NewContext(JS::DefaultHeapMaxBytes);
  if (!cx) {
    return false;
  }
  CONTEXT = cx;
  SCRIPT_VALUE.init(cx);

  if (!js::UseInternalJobQueues(cx) || !JS::InitSelfHostedCode(cx)) {
    return false;
  }

  bool ENABLE_PBL = std::string(std::getenv("ENABLE_PBL")) == "1";
  if (ENABLE_PBL) {
    JS_SetGlobalJitCompilerOption(cx, JSJitCompilerOption::JSJITCOMPILER_PORTABLE_BASELINE_ENABLE,
                                  1);
    JS_SetGlobalJitCompilerOption(
        cx, JSJitCompilerOption::JSJITCOMPILER_PORTABLE_BASELINE_WARMUP_THRESHOLD, 0);
  }

  if (!create_content_global(cx) || !create_initializer_global(ENGINE)) {
    return false;
  }

  JSAutoRealm ar(cx, GLOBAL);

  JS::SetPromiseRejectionTrackerCallback(cx, rejection_tracker);
  unhandledRejectedPromises.init(cx, JS::NewSetObject(cx));
  if (!unhandledRejectedPromises) {
    return false;
  }

  auto opts = new JS::CompileOptions(cx);

  // This ensures that we're eagerly loading the sript, and not lazily
  // generating bytecode for functions.
  // https://searchfox.org/mozilla-central/rev/5b2d2863bd315f232a3f769f76e0eb16cdca7cb0/js/public/CompileOptions.h#571-574
  opts->setForceFullParse();
  scriptLoader = new ScriptLoader(ENGINE, opts, config.path_prefix);

  // TODO: restore in a way that doesn't cause a dependency on the Performance builtin in the core runtime.
  //   builtins::Performance::timeOrigin.emplace(
  //       std::chrono::high_resolution_clock::now());

  return true;
}

static bool report_unhandled_promise_rejections(JSContext *cx) {
  RootedValue iterable(cx);
  if (!JS::SetValues(cx, unhandledRejectedPromises, &iterable))
    return false;

  JS::ForOfIterator it(cx);
  if (!it.init(iterable))
    return false;

  RootedValue promise_val(cx);
  RootedObject promise(cx);
  while (true) {
    bool done;
    if (!it.next(&promise_val, &done))
      return false;

    if (done)
      break;

    promise = &promise_val.toObject();
    // Note: we unconditionally print these, since they almost always indicate
    // serious bugs.
    fprintf(stderr, "Promise rejected but never handled: ");
    RootedValue result(cx, JS::GetPromiseResult(promise));
    dump_promise_rejection(cx, result, promise, stderr);
  }

  return true;
}

static void DumpPendingException(JSContext *cx, const char *description, FILE *fp) {
  JS::ExceptionStack exception(cx);
  if (!JS::StealPendingExceptionStack(cx, &exception)) {
    fprintf(fp,
            "Error: exception pending after %s, but got another error "
            "when trying to retrieve it. Aborting.\n",
            description);
  } else {
      fprintf(fp, "Exception while %s\n", description);
    JS::ErrorReportBuilder report(cx);
    if (!report.init(cx, exception, JS::ErrorReportBuilder::WithSideEffects)) {
      fprintf(fp, "unable to build error report");
    } else {
      JS::PrintError(fp, report, false);
    }
  }
}

static void abort(JSContext *cx, const char *description) {
  // Note: we unconditionally print messages here, since they almost always
  // indicate serious bugs.
  if (JS_IsExceptionPending(cx)) {
    DumpPendingException(cx, description, stderr);
  } else {
    fprintf(stderr,
            "Error while %s, but no exception is pending. "
            "Aborting, since that doesn't seem recoverable at all.\n",
            description);
  }

  if (JS::SetSize(cx, unhandledRejectedPromises) > 0) {
    fprintf(stderr, "Additionally, some promises were rejected, but the "
                    "rejection never handled:\n");
    report_unhandled_promise_rejections(cx);
  }

  fflush(stderr);
  exit(1);
}

extern bool install_builtins(Engine *engine);

#ifdef DEBUG
static bool trap(JSContext *cx, unsigned argc, JS::Value *vp) {
  JS::CallArgs args = CallArgsFromVp(argc, vp);
  ENGINE->dump_value(args.get(0));
  MOZ_ASSERT(false, "trap function called");
  return false;
}
#endif

Engine::Engine(std::unique_ptr<EngineConfig> config) {
  // total_compute = 0;
  MOZ_ASSERT(!ENGINE);
  ENGINE = this;
  config_ = std::move(config);

  TRACE("StarlingMonkey engine initializing");

  if (!init_js(*config_.get())) {
    abort("Initializing JS Engine");
  }

  JS_SetContextPrivate(cx(), this);
  JS::EnterRealm(cx(), global());
  core::EventLoop::init(cx());

  if (!install_builtins(this)) {
    abort("installing builtins");
  }

  TRACE("Builtins installed");

#ifdef DEBUG
  if (!JS_DefineFunction(cx(), global(), "trap", trap, 1, 0)) {
    abort("installing trap function");
  }
#endif

  if (config_->initializer_script_path) {
    if (!run_initialization_script()) {
      abort("running initialization script");
    }
  }

  TRACE("Module mode: " << config_->module_mode);
  scriptLoader->enable_module_mode(config_->module_mode);

  mozilla::Maybe<std::string_view> content_script_path = config_->content_script_path;

  if (config_->pre_initialize) {
    state_ = EngineState::ScriptPreInitializing;
  } else {
    state_ = EngineState::Initialized;
    // Debugging isn't supported during wizening, so only try it when doing runtime evaluation.
    // The debugger can be initialized at runtime by whatever export is invoked on the
    // resumed wizer snapshot.
    content_debugger::maybe_init_debugger(this, false);
    if (auto replacement_script_path = content_debugger::replacement_script_path()) {
      TRACE("Using replacement script path received from debugger: " << *replacement_script_path);
      content_script_path = replacement_script_path;
    }
  }

  if (content_script_path) {
    TRACE("Evaluating initial script from file " << content_script_path.value());
    RootedValue result(cx());
    if (!eval_toplevel(content_script_path.value().data(), &result)) {
      abort("evaluating top-level script");
    }
  }

  if (config_->content_script) {
    TRACE("Evaluating initial inline script");
    JS::SourceText<mozilla::Utf8Unit> source;
    std::string path = "<eval>";
    const std::string& script = config_->content_script.value();
    RootedValue result(cx());
    if (!source.init(cx(), script.c_str(), script.size(), JS::SourceOwnership::Borrowed)
        || !eval_toplevel(source, path.data(), &result)) {
      abort("evaluating top-level inline script");
    }
  }
}

JSContext *Engine::cx() { return CONTEXT; }

Engine *Engine::get(JSContext *cx) {
  return static_cast<Engine *>(JS_GetContextPrivate(cx));
}

HandleObject Engine::global() { return GLOBAL; }
EngineState Engine::state() { return state_; }
bool Engine::debugging_enabled() {
  return config_->debugging;
}

void Engine::finish_pre_initialization() {
  MOZ_ASSERT(state_ == EngineState::ScriptPreInitializing);
  js::ResetMathRandomSeed(ENGINE->cx());
  state_ = EngineState::Initialized;
}

HandleValue Engine::script_value() {
  return SCRIPT_VALUE;
}

void Engine::abort(const char *reason) {
  state_ = EngineState::Aborted;
  ::abort(CONTEXT, reason);
}

bool Engine::define_builtin_module(const char* id, HandleValue builtin) {
  TRACE("Defining builtin module '" << id << "'");
  return scriptLoader->define_builtin_module(id, builtin);
}

static bool define_builtin_module(JSContext *cx, unsigned argc, Value *vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  auto name = core::encode(cx, args.get(0));
  if (!name) {
    return false;
  }
  if (!args.get(1).isObject()) {
    JS_ReportErrorUTF8(cx, "Second argument to defineBuiltinModule must be an object");
    return false;
  }
  if (!Engine::get(cx)->define_builtin_module(name.ptr.get(), args.get(1))) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

bool Engine::run_initialization_script() {
  auto cx = this->cx();

  JSAutoRealm ar(cx, INIT_SCRIPT_GLOBAL);

  auto path = config_->initializer_script_path.value();
  TRACE("Running initialization script from file " << path);
  JS::SourceText<mozilla::Utf8Unit> source;
  if (!scriptLoader->load_resolved_script(CONTEXT, path.c_str(), path.c_str(), source)) {
    return false;
  }
  auto opts = new JS::CompileOptions(cx);
  opts->setDiscardSource();
  opts->setFile(path.c_str());
  JS::RootedScript script(cx, Compile(cx, *opts, source));
  if (!script) {
    return false;
  }
  RootedValue result(cx);
  return JS_ExecuteScript(cx, script, &result);
}

HandleObject Engine::init_script_global() { return INIT_SCRIPT_GLOBAL; }

bool Engine::eval_toplevel(JS::SourceText<mozilla::Utf8Unit> &source, const char *path,
                           MutableHandleValue result) {
  MOZ_ASSERT(state() > EngineState::EngineInitializing, "Engine must be done initializing");
  MOZ_ASSERT(state() != EngineState::Aborted, "Engine state is aborted");

  RootedValue ns(cx());
  RootedValue tla_promise(cx());
  if (!scriptLoader->eval_top_level_script(path, source, &ns, &tla_promise)) {
    return false;
  }

  SCRIPT_VALUE = ns;
  this->run_event_loop();

  // TLA rejections during pre-initialization are treated as top-level exceptions.
  // TLA may remain unresolved, in which case it will continue tasks at runtime.
  // Rejections after pre-intialization remain unhandled rejections for now.
  if (tla_promise.isObject()) {
    RootedObject promise_obj(cx(), &tla_promise.toObject());
    JS::PromiseState state = JS::GetPromiseState(promise_obj);
    if (state == JS::PromiseState::Rejected) {
      RootedValue err(cx(), JS::GetPromiseResult(promise_obj));
      JS_SetPendingException(cx(), err);
      return false;
    }
  }

  // Report any promise rejections that weren't handled before snapshotting.
  // TODO: decide whether we should abort in this case, instead of just
  // reporting.
  if (JS::SetSize(cx(), unhandledRejectedPromises) > 0) {
    report_unhandled_promise_rejections();
  }

  // TODO(performance): check if it makes sense to increase the empty chunk
  // count *before* running GC like this. The working theory is that otherwise
  // the engine might mark chunk pages as free that then later the allocator
  // doesn't turn into chunks without further fragmentation. But that might be
  // wrong. https://github.com/fastly/js-compute-runtime/issues/223
  // JS_SetGCParameter(cx, JSGC_MAX_EMPTY_CHUNK_COUNT, 10);

  // TODO(performance): verify that it's better to *not* perform a shrinking GC
  // here, as manual testing indicates. Running a shrinking GC here causes
  // *more* 4kb pages to be written to when processing a request, at least for
  // one fairly large input script.
  //
  // A hypothesis for why this is the case could be that most writes are to
  // object kinds that are initially allocated in the same vicinity, but that
  // the shrinking GC causes them to be intermingled with other objects. I.e.,
  // writes become more fragmented due to the shrinking GC.
  // https://github.com/fastly/js-compute-runtime/issues/224
  if (state() == EngineState::ScriptPreInitializing) {
    JS::PrepareForFullGC(cx());
    JS::NonIncrementalGC(cx(), JS::GCOptions::Normal, JS::GCReason::API);
  }

  // Ignore the first GC, but then print all others, because ideally GCs
  // should be rare, and developers should know about them.
  // TODO: consider exposing a way to parameterize this, and/or specifying a
  // dedicated log target for telemetry messages like this.
  JS_SetGCCallback(cx(), gc_callback, nullptr);

  return true;
}

bool Engine::eval_toplevel(const char *path, MutableHandleValue result) {
  JS::SourceText<mozilla::Utf8Unit> source;
  if (!scriptLoader->load_script(CONTEXT, path, source)) {
    return false;
  }

  return eval_toplevel(source, path, result);
}

bool Engine::run_event_loop() {
  return core::EventLoop::run_event_loop(this, 0);
}

void Engine::incr_event_loop_interest() {
  return core::EventLoop::incr_event_loop_interest();
}

void Engine::decr_event_loop_interest() {
  return core::EventLoop::decr_event_loop_interest();
}

bool Engine::dump_value(JS::Value val, FILE *fp) { return ::dump_value(CONTEXT, val, fp); }
bool Engine::print_stack(FILE *fp) { return ::print_stack(CONTEXT, fp); }

void Engine::dump_pending_exception(const char *description, FILE *fp) {
  DumpPendingException(CONTEXT, description, fp);
}

void Engine::dump_error(JS::HandleValue err, FILE *fp) {
  bool has_stack;
  ::dump_error(CONTEXT, err, &has_stack, fp);
  fflush(fp);
}

void Engine::dump_promise_rejection(HandleValue reason, HandleObject promise, FILE *fp) {
  ::dump_promise_rejection(CONTEXT, reason, promise, fp);
}

bool Engine::debug_logging_enabled() { return ::debug_logging_enabled(); }

bool Engine::has_pending_async_tasks() { return core::EventLoop::has_pending_async_tasks(); }

void Engine::queue_async_task(AsyncTask *task) { core::EventLoop::queue_async_task(task); }
bool Engine::cancel_async_task(AsyncTask *task) {
  return core::EventLoop::cancel_async_task(this, task);
}

bool Engine::has_unhandled_promise_rejections() {
  return JS::SetSize(CONTEXT, unhandledRejectedPromises) > 0;
}
void Engine::report_unhandled_promise_rejections() {
  ::report_unhandled_promise_rejections(this->cx());
}
void Engine::clear_unhandled_promise_rejections() {
  JS::SetClear(CONTEXT, unhandledRejectedPromises);
}
