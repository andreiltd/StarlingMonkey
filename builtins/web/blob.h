#ifndef BUILTINS_WEB_BLOB_H
#define BUILTINS_WEB_BLOB_H

#include "builtin.h"
#include "extension-api.h"
#include "js/TypeDecls.h"

namespace builtins {
namespace web {
namespace blob {

class Blob : public FinalizableBuiltinImpl<Blob> {
  static bool arrayBuffer(JSContext *cx, unsigned argc, JS::Value *vp);
  static bool bytes(JSContext *cx, unsigned argc, JS::Value *vp);
  static bool slice(JSContext *cx, unsigned argc, JS::Value *vp);
  static bool text(JSContext *cx, unsigned argc, JS::Value *vp);

  static bool size_get(JSContext *cx, unsigned argc, JS::Value *vp);
  static bool type_get(JSContext *cx, unsigned argc, JS::Value *vp);

  static JSObject *data_to_owned_array_buffer(JSContext *cx, HandleObject self);

public:
  static constexpr const char *class_name = "Blob";

  static const JSFunctionSpec static_methods[];
  static const JSPropertySpec static_properties[];
  static const JSFunctionSpec methods[];
  static const JSPropertySpec properties[];

  static constexpr unsigned ctor_length = 1;
  enum Slots { Data, Type, Endings, Count };
  enum Endings { Transparent, Native };

  using ByteBuffer = std::vector<uint8_t>;

  static ByteBuffer *blob(JSObject *self);
  static JSString *type(JSObject *self);
  static bool init_blob_parts(JSContext *cx, HandleObject self, HandleValue iterable);
  static bool init_options(JSContext *cx, HandleObject self, HandleValue opts);

  static JSObject *create(JSContext *cx, std::unique_ptr<ByteBuffer> data, JSString *type);

  static bool init_class(JSContext *cx, HandleObject global);
  static bool constructor(JSContext *cx, unsigned argc, Value *vp);
  static void finalize(JS::GCContext *gcx, JSObject *obj);
};

bool install(api::Engine *engine);

} // namespace blob
} // namespace web
} // namespace builtins

#endif // BUILTINS_WEB_URL_H