#include "blob.h"
#include "builtin.h"
#include "encode.h"
#include "extension-api.h"
#include "rust-encoding.h"
#include "streams/native-stream-source.h"

#include "js/ArrayBuffer.h"
#include "js/Conversions.h"
#include "js/experimental/TypedData.h"
#include "js/Stream.h"
#include "js/TypeDecls.h"
#include "js/Value.h"

namespace builtins {
namespace web {
namespace blob {

JSObject* new_array_buffer_from_span(JSContext *cx, std::span<const uint8_t> span);

static api::Engine *ENGINE;

class StreamTask final : public api::AsyncTask {
    Heap<JSObject *> source_;

    static constexpr size_t CHUNK_SIZE = 8192;

public:
  explicit StreamTask(const HandleObject source, api::Engine *engine)  : source_(source) {
    engine->incr_event_loop_interest();
    handle_ = IMMEDIATE_TASK_HANDLE;
  }

  [[nodiscard]] bool run(api::Engine *engine) override {
    JSContext *cx = engine->cx();
    RootedObject owner(cx, streams::NativeStreamSource::owner(source_));
    RootedObject controller(cx, streams::NativeStreamSource::controller(source_));
    RootedValue ret(cx);

    auto rdr = Blob::reader(owner);
    auto maybe_chunk = rdr->read(CHUNK_SIZE);

    if (!maybe_chunk.has_value()) {
      if (!JS::Call(cx, controller, "close", HandleValueArray::empty(), &ret)) {
        return false;
      }

      rdr->seek(0);
      return cancel(engine);
    }

    MOZ_ASSERT(maybe_chunk.has_value());
    auto chunk = maybe_chunk.value();
    auto chunk_size = chunk.size();

    RootedObject array_buffer(cx, new_array_buffer_from_span(cx, chunk));
    if (!array_buffer) {
      return false;
    }

    RootedObject bytes_buffer(cx, JS_NewUint8ArrayWithBuffer(cx, array_buffer, 0, chunk_size));
    if (!bytes_buffer) {
      return false;
    }

    RootedValueArray<1> enqueue_args(cx);
    enqueue_args[0].setObject(*bytes_buffer);
    if (!JS::Call(cx, controller, "enqueue", enqueue_args, &ret)) {
      return false;
    }

    return cancel(engine);
  }

  [[nodiscard]] uint64_t deadline() override {
    return host_api::MonotonicClock::now();
  }

  [[nodiscard]] bool cancel(api::Engine *engine) override {
    handle_ = INVALID_POLLABLE_HANDLE;
    engine->decr_event_loop_interest();
    return true;
  }

  void trace(JSTracer *trc) override { TraceEdge(trc, &source_, "source for future"); }
};

const JSFunctionSpec Blob::static_methods[] = {
    JS_FS_END,
};

const JSPropertySpec Blob::static_properties[] = {
    JS_PS_END,
};

const JSFunctionSpec Blob::methods[] = {
    JS_FN("arrayBuffer", Blob::arrayBuffer, 0, JSPROP_ENUMERATE),
    JS_FN("bytes", Blob::bytes, 0, JSPROP_ENUMERATE),
    JS_FN("slice", Blob::slice, 0, JSPROP_ENUMERATE),
    JS_FN("stream", Blob::stream, 0, JSPROP_ENUMERATE),
    JS_FN("text", Blob::text, 0, JSPROP_ENUMERATE),
    JS_FS_END,
};

const JSPropertySpec Blob::properties[] = {
    JS_PSG("size", size_get, JSPROP_ENUMERATE),
    JS_PSG("type", type_get, JSPROP_ENUMERATE),
    JS_STRING_SYM_PS(toStringTag, "Blob", JSPROP_READONLY),
    JS_PS_END,
};


JSObject* new_array_buffer_from_span(JSContext *cx, std::span<const uint8_t> span) {
  mozilla::UniquePtr<uint8_t[], JS::FreePolicy> buf{
      static_cast<uint8_t*>(JS_malloc(cx, span.size()))};
  if (!buf) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }

  memcpy(buf.get(), span.data(), span.size());

  auto array_buffer = JS::NewArrayBufferWithContents(
      cx, span.size(), buf.get(), JS::NewArrayBufferOutOfMemory::CallerMustFreeMemory);
  if (!array_buffer) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }

  // `array_buffer` now owns `buf`
  static_cast<void>(buf.release());
  return array_buffer;
}

template <typename T> bool validate_type(T *chars, size_t strlen) {
  for (size_t i = 0; i < strlen; i++) {
    T c = chars[i];
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }

  return true;
}

// 1. If type contains any characters outside the range U+0020 to U+007E, then set t to the empty string.
// 2. Convert every character in type to ASCII lowercase.
JSString *normalize_type(JSContext *cx, HandleValue value) {
  JS::RootedString value_str(cx);

  if (value.isObject() || value.isString()) {
    value_str = JS::ToString(cx, value);
    if (!value_str) {
      return nullptr;
    }
  } else if (value.isNull()) {
    return JS::ToString(cx, value);
  } else {
    return JS_GetEmptyString(cx);
  }

  auto str = JS::StringToLinearString(cx, value_str);
  if (!str) {
    return nullptr;
  }

  std::string normalized;
  auto strlen = JS::GetLinearStringLength(str);

  if (!strlen) {
    return JS_GetEmptyString(cx);
  }

  if (JS::LinearStringHasLatin1Chars(str)) {
    JS::AutoCheckCannotGC nogc(cx);
    auto chars = JS::GetLatin1LinearStringChars(nogc, str);
    if (!validate_type(chars, strlen)) {
      return JS_GetEmptyString(cx);
    }

    normalized = std::string(reinterpret_cast<const char *>(chars), strlen);
  } else {
    JS::AutoCheckCannotGC nogc(cx);
    auto chars = (JS::GetTwoByteLinearStringChars(nogc, str));
    if (!validate_type(chars, strlen)) {
      return JS_GetEmptyString(cx);
    }

    normalized.reserve(strlen);
    for (size_t i = 0; i < strlen; ++i) {
      normalized += static_cast<unsigned char>(chars[i]);
    }
  }

  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  return JS_NewStringCopyN(cx, normalized.c_str(), normalized.length());
}

JSObject *Blob::data_to_owned_array_buffer(JSContext *cx, HandleObject self) {
    size_t total_size = blob_size(self);
    size_t bytes_read = 0;

    return Blob::data_to_owned_array_buffer(cx, self, 0, total_size, &bytes_read);
}

JSObject* Blob::data_to_owned_array_buffer(JSContext* cx, HandleObject self, size_t offset, size_t size, size_t* bytes_read) {
  auto blob = Blob::blob(self);
  auto blob_size = blob->size();
  *bytes_read = 0;

  MOZ_ASSERT(offset <= blob_size);

  size_t available_bytes = blob_size - offset;
  size_t read_size = std::min(size, available_bytes);

  auto span = std::span<uint8_t>(blob->data() + offset, read_size);
  auto array_buffer = new_array_buffer_from_span(cx, span);
  if (!array_buffer) {
    return nullptr;
  }

  *bytes_read = read_size;
  return array_buffer;
}

bool Blob::arrayBuffer(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(0)

  JS::RootedObject promise(cx, JS::NewPromiseObject(cx, nullptr));
  if (!promise) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  args.rval().setObject(*promise);

  auto buffer = data_to_owned_array_buffer(cx, self);
  if (!buffer) {
    return RejectPromiseWithPendingError(cx, promise);
  }

  JS::RootedValue result(cx);
  result.setObject(*buffer);
  JS::ResolvePromise(cx, promise, result);

  return true;
}

bool Blob::bytes(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(0)

  JS::RootedObject promise(cx, JS::NewPromiseObject(cx, nullptr));
  if (!promise) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  args.rval().setObject(*promise);

  JS::RootedObject buffer(cx, data_to_owned_array_buffer(cx, self));
  if (!buffer) {
    return RejectPromiseWithPendingError(cx, promise);
  }

  auto len = JS::GetArrayBufferByteLength(buffer);
  JS::RootedObject byte_array(cx, JS_NewUint8ArrayWithBuffer(cx, buffer, 0, len));
  if (!byte_array) {
    return RejectPromiseWithPendingError(cx, promise);
  }

  JS::RootedValue result(cx);
  result.setObject(*byte_array);
  JS::ResolvePromise(cx, promise, result);

  return true;
}

bool Blob::slice(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(0)

  auto src = Blob::blob(self);
  int64_t size = src->size();
  int64_t start = 0;
  int64_t end = size;

  JS::RootedString contentType(cx, JS_GetEmptyString(cx));

  if (args.hasDefined(0)) {
    HandleValue start_val = args.get(0);
    if (!JS::ToInt64(cx, start_val, &start)) {
      return false;
    }
  }

  if (args.hasDefined(1)) {
    HandleValue end_val = args.get(1);
    if (!JS::ToInt64(cx, end_val, &end)) {
      return false;
    }
  }

  if (args.hasDefined(2)) {
    HandleValue contentType_val = args.get(2);
    if (!(contentType = normalize_type(cx, contentType_val))) {
      return false;
    }
  }

  // A negative value, is treated as an offset from the end of the Blob toward the beginning
  start = (start < 0) ? std::max((size + start), 0LL) : std::min(start, size);
  end = (end < 0) ? std::max((size + end), 0LL) : std::min(end, size);

  auto dst = (end - start > 0)
                 ? std::make_unique<std::vector<uint8_t>>(src->begin() + start, src->begin() + end)
                 : std::make_unique<std::vector<uint8_t>>();

  JS::RootedObject new_blob(cx, create(cx, std::move(dst), contentType));
  if (!new_blob) {
    return false;
  }

  args.rval().setObject(*new_blob);
  return true;
}

bool Blob::stream(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(0)

  auto native_stream = streams::NativeStreamSource::create(
    cx, self, JS::UndefinedHandleValue, stream_pull, stream_cancel);

  JS::RootedObject source(cx, native_stream);
  if (!source) {
    return false;
  }

  JS::RootedObject src_stream(cx, JS::NewReadableDefaultStreamObject(cx, source, nullptr, 0.0));
  if (!src_stream) {
    return false;
  }

  args.rval().setObject(*src_stream);
  return true;
}

bool Blob::text(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(0)

  JS::RootedObject promise(cx, JS::NewPromiseObject(cx, nullptr));
  if (!promise) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  args.rval().setObject(*promise);

  auto src = Blob::blob(self);
  auto encoding = const_cast<jsencoding::Encoding *>(jsencoding::encoding_for_label_no_replacement(
      reinterpret_cast<uint8_t *>(const_cast<char *>("UTF-8")), 5));

  auto decoder = jsencoding::encoding_new_decoder_with_bom_removal(encoding);
  MOZ_ASSERT(decoder);

  auto src_len = src->size();
  auto dst_len = jsencoding::decoder_max_utf16_buffer_length(decoder, src_len);

  JS::UniqueTwoByteChars dst(new char16_t[dst_len + 1]);
  if (!dst) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  bool had_replacements;
  auto dst_data =  reinterpret_cast<uint16_t *>(dst.get());

  jsencoding::decoder_decode_to_utf16(decoder, src->data(), &src_len, dst_data, &dst_len, true,
                                      &had_replacements);


  JS::RootedString str(cx, JS_NewUCString(cx, std::move(dst), dst_len));
  if (!str) {
    return RejectPromiseWithPendingError(cx, promise);
  }

  JS::RootedValue result(cx);
  result.setString(str);
  JS::ResolvePromise(cx, promise, result);

  return true;
}

bool Blob::size_get(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(0);
  // TODO: Change this class so that its prototype isn't an instance of the class
  if (self == proto_obj) {
    return api::throw_error(cx, api::Errors::WrongReceiver, "size get", "Blob");
  }

  auto size = Blob::blob_size(self);
  args.rval().setNumber(size);
  return true;
}

bool Blob::type_get(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(0);
  // TODO: Change this class so that its prototype isn't an instance of the class
  if (self == proto_obj) {
    return api::throw_error(cx, api::Errors::WrongReceiver, "type get", "Blob");
  }

  auto type = Blob::type(self);
  args.rval().setString(type);
  return true;
}

bool Blob::stream_cancel(JSContext *cx, JS::CallArgs args, JS::HandleObject stream,
                         JS::HandleObject owner, JS::HandleValue reason) {
  args.rval().setUndefined();
  return true;
}

bool Blob::stream_pull(JSContext *cx, JS::CallArgs args, JS::HandleObject source,
                       JS::HandleObject owner, JS::HandleObject controller) {

  ENGINE->queue_async_task(new StreamTask(source, ENGINE));

  args.rval().setUndefined();
  return true;
}

std::vector<uint8_t> *Blob::blob(JSObject *self) {
  auto blob = static_cast<std::vector<uint8_t> *>(
      JS::GetReservedSlot(self, static_cast<size_t>(Blob::Slots::Data)).toPrivate());

  MOZ_ASSERT(blob);
  return blob;
}

size_t Blob::blob_size(JSObject *self) {
  auto blob = static_cast<std::vector<uint8_t> *>(
      JS::GetReservedSlot(self, static_cast<size_t>(Blob::Slots::Data)).toPrivate());

  MOZ_ASSERT(blob);
  return blob->size();
}

JSString *Blob::type(JSObject *self) {
  auto type = static_cast<JSString *>(
      JS::GetReservedSlot(self, static_cast<size_t>(Blob::Slots::Type)).toPrivate());

  MOZ_ASSERT(type);
  return type;
}

BlobReader *Blob::reader(JSObject *self) {
  auto maybe_reader = static_cast<BlobReader *>(
      JS::GetReservedSlot(self, static_cast<size_t>(Blob::Slots::Reader)).toPrivate());

  if (maybe_reader != nullptr) {
    return maybe_reader;
  } else {
    auto blob = Blob::blob(self);
    auto span = std::span<uint8_t>(blob->data(), blob->size());
    auto reader = new BlobReader(span);

    SetReservedSlot(self, static_cast<uint32_t>(Slots::Reader), PrivateValue(reader));
    return reader;
  }
}

bool Blob::append_value(JSContext *cx, HandleObject self, HandleValue val) {
  auto blob = Blob::blob(self);

  if (val.isObject()) {
    RootedObject obj(cx, &val.toObject());

    if (Blob::is_instance(obj)) {
      auto src = Blob::blob(obj);
      blob->insert(blob->end(), src->begin(), src->end());
      return true;
    } else if (JS_IsArrayBufferViewObject(obj) || JS::IsArrayBufferObject(obj)) {
      auto span = value_to_buffer(cx, val, "Blob Parts");
      if (span.has_value()) {
        blob->insert(blob->end(), span->begin(), span->end());
      }
      return true;
    }
  } else if (val.isString()) {
    auto chars = core::encode(cx, val);
    if (!chars) {
      return false;
    }

    // TODO: convert line endings: https://w3c.github.io/FileAPI/#convert-line-endings-to-native
    auto src = chars.ptr.get();
    auto len = chars.len;
    blob->insert(blob->end(), src, src + len);
    return true;
  }

  // FALLBACK: if we ever get here convert, to string and call append again
  auto str = JS::ToString(cx, val);
  if (!str) {
    return false;
  }

  RootedValue str_val(cx, JS::StringValue(str));
  return append_value(cx, self, str_val);
}

bool Blob::init_blob_parts(JSContext *cx, HandleObject self, HandleValue value) {
  JS::ForOfIterator it(cx);
  if (!it.init(value, JS::ForOfIterator::AllowNonIterable)) {
    return false;
  }

  bool is_typed_array = value.isObject() && JS_IsTypedArrayObject(&value.toObject());
  bool is_iterable = value.isObject() && it.valueIsIterable();

  if (is_typed_array) {
    // append typed array value directly...
    return append_value(cx, self, value);
  } else if (is_iterable) {
    // if the object is an iterable, walk over its elements...
    JS::Rooted<JS::Value> item(cx);
    while (true) {
      bool done;

      if (!it.next(&item, &done)) {
        return false;
      }
      if (done) {
        break;
      }

      if (!append_value(cx, self, item)) {
        return false;
      }
    }

    return true;
  } else {
    // non-objects are not allowed for the blobParts
    return api::throw_error(cx, api::Errors::TypeError, "Blob.constructor", "blobParts", "be an object");
  }
}

bool Blob::init_options(JSContext *cx, HandleObject self, HandleValue initv) {
  JS::RootedValue init_val(cx, initv);

  if (!init_val.isObject()) {
    return api::throw_error(cx, api::Errors::TypeError, "Blob.constructor", "options", "be an object");
  }

  // `options` is an object which may specify any of the properties:
  // - `type`: the MIME type of the data that will be stored into the blob,
  // - `endings`: how to interpret newline characters (\n) within the contents.
  JS::RootedObject opts(cx, init_val.toObjectOrNull());
  bool has_endings, has_type;

  if (!JS_HasProperty(cx, opts, "endings", &has_endings) ||
      !JS_HasProperty(cx, opts, "type", &has_type)) {
    return false;
  }

  if (!has_type && !has_endings) {
    // Use defaults
    return true;
  }

  if (has_endings) {
    JS::RootedValue endings_val(cx);
    bool is_transparent, is_native;
    if (!JS_GetProperty(cx, opts, "endings", &endings_val)) {
      return false;
    }

    auto endings_str = JS::ToString(cx, endings_val);
    if (!JS_StringEqualsLiteral(cx, endings_str, "transparent", &is_transparent) ||
        !JS_StringEqualsLiteral(cx, endings_str, "native", &is_native)) {
      return false;
    }

    if (is_transparent || is_native) {
      auto endings = is_native ? Blob::Endings::Native : Blob::Endings::Transparent;
      SetReservedSlot(self, static_cast<uint32_t>(Slots::Endings), JS::Int32Value(endings));
    }
  }

  if (has_type) {
    JS::RootedValue type(cx);
    if (!JS_GetProperty(cx, opts, "type", &type)) {
      return false;
    }

    auto type_str = normalize_type(cx, type);
    if (!type_str) {
      return false;
    }
    SetReservedSlot(self, static_cast<uint32_t>(Slots::Type), PrivateValue(type_str));
  }

  return true;
}

JSObject *Blob::create(JSContext *cx, std::unique_ptr<Blob::ByteBuffer> data, JSString *type) {
  JSObject *self = JS_NewObjectWithGivenProto(cx, &class_, proto_obj);
  if (!self) {
    return nullptr;
  }

  SetReservedSlot(self, static_cast<uint32_t>(Slots::Data), PrivateValue(data.release()));
  SetReservedSlot(self, static_cast<uint32_t>(Slots::Type), PrivateValue(type));
  SetReservedSlot(self, static_cast<uint32_t>(Slots::Endings), JS::Int32Value(Blob::Endings::Transparent));
  SetReservedSlot(self, static_cast<uint32_t>(Slots::Reader), PrivateValue(nullptr));
  return self;
}

bool Blob::constructor(JSContext *cx, unsigned argc, JS::Value *vp) {
  CTOR_HEADER("Blob", 0);

  RootedValue blobParts(cx, args.get(0));
  RootedValue opts(cx, args.get(1));
  RootedObject self(cx, JS_NewObjectForConstructor(cx, &class_, args));

  if (!self) {
    return false;
  }

  SetReservedSlot(self, static_cast<uint32_t>(Slots::Data), PrivateValue(new std::vector<uint8_t>()));
  SetReservedSlot(self, static_cast<uint32_t>(Slots::Type), PrivateValue(JS_GetEmptyString(cx)));
  SetReservedSlot(self, static_cast<uint32_t>(Slots::Endings), JS::Int32Value(Blob::Endings::Transparent));
  SetReservedSlot(self, static_cast<uint32_t>(Slots::Reader), PrivateValue(nullptr));

  // walk the blob parts and write it to concatenated buffer
  if (blobParts.isNull()) {
    return api::throw_error(cx, api::Errors::TypeError, "Blob.constructor", "blobParts", "be an object");
  }

  if (!blobParts.isUndefined() && !init_blob_parts(cx, self, blobParts)) {
    return false;
  }

  if (!opts.isNullOrUndefined() && !init_options(cx, self, opts)) {
    return false;
  }

  args.rval().setObject(*self);
  return true;
}

bool Blob::init_class(JSContext *cx, JS::HandleObject global) {
  return init_class_impl(cx, global);
}

void Blob::finalize(JS::GCContext *gcx, JSObject *self) {
  auto blob = Blob::blob(self);
  if (blob) {
    free(blob);
  }
}

bool install(api::Engine *engine) {
  ENGINE = engine;
  return Blob::init_class(engine->cx(), engine->global());
}

} // namespace blob
} // namespace web
} // namespace builtins
