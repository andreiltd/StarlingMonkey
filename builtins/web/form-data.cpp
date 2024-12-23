#include "blob.h"
#include "builtin.h"
#include "encode.h"
#include "file.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "form-data.h"

namespace {

using builtins::web::form_data::FormDataEntry;

FormDataEntry entry_from_kv_pair(std::string name, HandleValue value) {
    FormDataEntry entry;
    entry.name = std::move(name);
    entry.value = value;

    return entry;
}

} // namespace

namespace builtins {
namespace web {
namespace form_data {

using blob::Blob;
using file::File;

const JSFunctionSpec FormData::static_methods[] = {
    JS_FS_END,
};

const JSPropertySpec FormData::static_properties[] = {
    JS_PS_END,
};

const JSFunctionSpec FormData::methods[] = {
    JS_FN("append", append, 0, JSPROP_ENUMERATE),
    JS_FN("delete", remove, 0, JSPROP_ENUMERATE),
    JS_FN("get", get, 0, JSPROP_ENUMERATE),
    JS_FN("getAll", FormData::getAll, 0, JSPROP_ENUMERATE),
    JS_FN("has", FormData::has, 0, JSPROP_ENUMERATE),
    JS_FN("set", FormData::set, 0, JSPROP_ENUMERATE),
    JS_FS_END,
};

const JSPropertySpec FormData::properties[] = {
    JS_PS_END,
};

FormData::EntryList* FormData::entry_list(JSObject *self) {
  MOZ_ASSERT(is_instance(self));

  auto entries = static_cast<EntryList *>(
      JS::GetReservedSlot(self, static_cast<size_t>(FormData::Slots::Entries)).toPrivate());

  MOZ_ASSERT(entries);
  return entries;
}

bool FormData::append(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(2)

  RootedValue name(cx, args.get(0));
  RootedValue value(cx, args.get(1));
  RootedValue filename(cx, args.get(2));

  if (!name.isString()) {
    return false;
  }

  auto chars = core::encode(cx, name);
  if (!chars) {
    return false;
  }

  std::string name_str(chars.ptr.get(), chars.len);
  auto entries = entry_list(self);

  switch(args.length()) {
  case 2: {
    if (!value.isString()) {
      return false;
    }

    auto entry = entry_from_kv_pair(std::move(name_str), value);
    entries->append(entry);
    break;
  }
  case 3: {
    if (!value.isObject()) {
      return false;
    }

    RootedObject obj(cx, &value.toObject());

    if (File::is_instance(obj)) {
      auto entry = entry_from_kv_pair(std::move(name_str), value);
      entries->append(entry);
    } else if (Blob::is_instance(obj)) {
      HandleValueArray arr = HandleValueArray(value);
      RootedObject file_bits(cx, NewArrayObject(cx, arr));
      if (!file_bits) {
        return false;
      }

      RootedValue file_bits_val(cx, JS::ObjectValue(*file_bits));
      RootedValue opts_val(cx, JS::NullValue());
      RootedValue filename_val(cx);

      if (filename.isNullOrUndefined()) {
        RootedString empty(cx, JS_NewStringCopyZ(cx, "blob"));
        if (!empty) {
          return false;
        }

        RootedValue empty_val(cx, JS::StringValue(empty));
        filename_val = empty_val;
      } else {
        filename_val = filename;
      }

      RootedObject file(cx, File::create(cx, file_bits_val, filename_val, opts_val));
      if (!file) {
        return false;
      }

      RootedValue file_val(cx, JS::ObjectValue(*file));
      auto entry = entry_from_kv_pair(std::move(name_str), file_val);
      entries->append(entry);
    } else {
      return false;
    }

    break;
  }
  default:
    return false;
  }

  return true;
}

bool FormData::remove(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(1)

  RootedValue name(cx, args.get(0));

  if (!name.isString()) {
    return false;
  }

  auto name_to_remove = core::encode(cx, name);
  if (!name_to_remove) {
    return false;
  }

  entry_list(self)->eraseIf([&](const FormDataEntry &entry) {
    return entry.name == std::string_view(name_to_remove);
  });

  return true;
}

bool FormData::get(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(1)

  RootedValue name(cx, args.get(0));

  if (!name.isString()) {
    return false;
  }

  auto name_to_get = core::encode(cx, name);
  if (!name_to_get) {
    return false;
  }

  auto entries = entry_list(self);
  auto it = std::find_if(entries->begin(), entries->end(), [&](const FormDataEntry &entry) {
    return entry.name == std::string_view(name_to_get);
  });

  if (it != entries->end()) {
    args.rval().set(it->value);
  } else {
    args.rval().setUndefined();
  }

  return true;
}

bool FormData::getAll(JSContext *cx, unsigned argc, JS::Value *vp) {
  return true;
}

bool FormData::has(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(1)

  RootedValue name(cx, args.get(0));

  if (!name.isString()) {
    return false;
  }

  auto name_to_find = core::encode(cx, name);
  if (!name_to_find) {
    return false;
  }

  auto entries = entry_list(self);
  auto it = std::find_if(entries->begin(), entries->end(), [&](const FormDataEntry &entry) {
    return entry.name == std::string_view(name_to_find);
  });

  args.rval().setBoolean(it != entries->end());
  return true;
}

bool FormData::set(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(1)

  RootedValue name(cx, args.get(0));
  RootedValue value(cx, args.get(1));

  if (!name.isString()) {
    return false;
  }

  auto name_to_find = core::encode(cx, name);
  if (!name_to_find) {
    return false;
  }

  auto entries = entry_list(self);
  auto it = std::find_if(entries->begin(), entries->end(), [&](const FormDataEntry &entry) {
    return entry.name == std::string_view(name_to_find);
  });

  if (it != entries->end()) {
    it->value = value;
  }

  return true;
}

bool FormData::constructor(JSContext *cx, unsigned argc, JS::Value *vp) {
  CTOR_HEADER("FormData", 0);

  RootedObject self(cx, JS_NewObjectForConstructor(cx, &class_, args));

  if (!self) {
    return false;
  }

  SetReservedSlot(self, static_cast<uint32_t>(Slots::Entries), JS::PrivateValue(new EntryList));

  args.rval().setObject(*self);
  return true;
}

void FormData::finalize(JS::GCContext *gcx, JSObject *self) {
  MOZ_ASSERT(is_instance(self));
  auto entries = entry_list(self);
  if (entries) {
    free(entries);
  }
}

void FormData::trace(JSTracer *trc, JSObject *self) {
  MOZ_ASSERT(is_instance(self));
  auto entries = entry_list(self);
  entries->trace(trc);
}

bool FormData::init_class(JSContext *cx, JS::HandleObject global) {
  return init_class_impl(cx, global);
}

bool install(api::Engine *engine) {
  return FormData::init_class(engine->cx(), engine->global());
}

} // namespace form_data
} // namespace web
} // namespace builtins
