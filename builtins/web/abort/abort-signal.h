#ifndef BUILTINS_WEB_ABORT_SIGNAL_H_
#define BUILTINS_WEB_ABORT_SIGNAL_H_

#include "builtin.h"
#include "weak-index-set.h"

#include "../event/event-target.h"

namespace builtins {
namespace web {
namespace abort {

using AlgorithmFn = bool (*)(JSContext *, std::span<HeapValue>);

struct AbortAlgorithm {
  AlgorithmFn algorithm;
  std::vector<HeapValue> args;

  bool run(JSContext *cx) {
    return algorithm(cx, args);
  }

  void trace(JSTracer *trc) {
    for (auto& arg : args) {
      JS::TraceEdge(trc, &arg, "AbortAlgorithm arg");
    }
  }
};

inline std::optional<AbortAlgorithm>
make_algorithm(JSContext* cx, AlgorithmFn func, JS::HandleValueVector args)
{
  std::vector<JS::Heap<Value>> vec {args.begin(), args.end()};
  return AbortAlgorithm{func, std::move(vec)};
}

class AbortSignal : public BuiltinImpl<AbortSignal, TraceableClassPolicy> {
  static bool aborted_get(JSContext *cx, unsigned argc, JS::Value *vp);
  static bool reason_get(JSContext *cx, unsigned argc, JS::Value *vp);
  static bool onabort_get(JSContext *cx, unsigned argc, JS::Value *vp);
  static bool onabort_set(JSContext *cx, unsigned argc, JS::Value *vp);

  static bool timeout(JSContext *cx, unsigned argc, JS::Value *vp);
  static bool abort(JSContext *cx, unsigned argc, JS::Value *vp);
  static bool any(JSContext *cx, unsigned argc, JS::Value *vp);
  static bool throwIfAborted(JSContext *cx, unsigned argc, JS::Value *vp);

  static bool on_timeout(JSContext *cx, unsigned argc, JS::Value *vp);
  static bool set_reason(JSContext *cx, HandleObject self, HandleValue reason);
  static bool abort(JSContext *cx, HandleObject self, HandleValue reason);
  static bool run_abort_steps(JSContext *cx, HandleObject self);

  using AlgorithmList = JS::GCVector<AbortAlgorithm, 0, js::SystemAllocPolicy>;

  static JSObject *create(JSContext *cx);

  static AlgorithmList *algorithms(JSObject *self);
  static WeakIndexSet *source_signals(JSObject *self);
  static WeakIndexSet *dependent_signals(JSObject *self);

  friend class AbortController;

public:
  static constexpr int ParentSlots = event::EventTarget::Slots::Count;
  enum Slots {
    Reason = ParentSlots,
    Algorithms,
    Dependent,
    SourceSignals,
    DependentSignals,
    Count
  };

  static constexpr const char *class_name = "AbortSignal";
  static constexpr unsigned ctor_length = 0;

  static JSString *abort_type_atom;

  static const JSFunctionSpec static_methods[];
  static const JSPropertySpec static_properties[];
  static const JSFunctionSpec methods[];
  static const JSPropertySpec properties[];

  static bool add_algorithm(JSObject *self, AbortAlgorithm algorithm);
  static bool is_dependent(JSObject *self);
  static bool is_aborted(JSObject *self);

  static JSObject *create_with_reason(JSContext *cx, HandleValue reason);
  static JSObject *create_with_timeout(JSContext *cx, HandleValue timeout);
  static JSObject *create_with_signals(JSContext *cx, HandleValueArray signals);

  static bool init_class(JSContext *cx, HandleObject global);
  static bool constructor(JSContext *cx, unsigned argc, Value *vp);
  static void finalize(JS::GCContext *gcx, JSObject *self);
  static void trace(JSTracer *trc, JSObject *self);
};

} // namespace abort
} // namespace web
} // namespace builtins

#endif // BUILTINS_WEB_ABORT_SIGNAL_H_
