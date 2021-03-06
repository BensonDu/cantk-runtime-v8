#include "Native.h"
#include "Config.h"
#include "V8Wrapper.h"
#include "parse_html.h"
#include "CanvasRenderingContext2d.h"

class SimpleArrayBufferAllocator : public ArrayBuffer::Allocator {
 public:
  virtual void* Allocate(size_t length) {
    void* data = AllocateUninitialized(length);
    return data == NULL ? data : memset(data, 0, length);
  }

  virtual void* AllocateUninitialized(size_t length) { 
  	return malloc(length); 
  }

  virtual void Free(void* data, size_t) { 
  	free(data); 
  }
};

static SimpleArrayBufferAllocator array_buffer_allocator;

template <class TypeName>
inline Local<TypeName> StrongPersistentToLocal(
    const Persistent<TypeName>& persistent) {
  return *reinterpret_cast<Local<TypeName>*>(
      const_cast<Persistent<TypeName>*>(&persistent));
}

template <class TypeName>
inline Local<TypeName> WeakPersistentToLocal(
    Isolate* isolate,
    const Persistent<TypeName>& persistent) {
  return Local<TypeName>::New(isolate, persistent);
}

template <class TypeName>
inline Local<TypeName> PersistentToLocal(
    Isolate* isolate,
    const Persistent<TypeName>& persistent) {
  if (persistent.IsWeak()) {
    return WeakPersistentToLocal(isolate, persistent);
  } else {
    return StrongPersistentToLocal(persistent);
  }
}

///////////////////////////////////////////////////////
// Extracts a C string from a V8 Utf8Value.
static const char* ToCString(const String::Utf8Value& value) {
  return *value ? *value : "<string conversion failed>";
}

static void ReportException(Isolate* isolate, TryCatch* try_catch) {
  HandleScope handle_scope(isolate);
  String::Utf8Value exception(try_catch->Exception());
  const char* exception_string = ToCString(exception);
  Handle<Message> message = try_catch->Message();
  if (message.IsEmpty()) {
    // V8 didn't provide any extra information about this error; just
    // print the exception.
    LOGE("%s\n", exception_string);
  } else {
    // Print (filename):(line number): (message).
    String::Utf8Value filename(message->GetScriptOrigin().ResourceName());
    const char* filename_string = ToCString(filename);
    int linenum = message->GetLineNumber();
    LOGE("%s:%i: %s\n", filename_string, linenum, exception_string);
    // Print line of source code.
    String::Utf8Value sourceline(message->GetSourceLine());
    const char* sourceline_string = ToCString(sourceline);
    LOGE("%s\n", sourceline_string);
    // Print wavy underline (GetUnderline is deprecated).
    int start = message->GetStartColumn();
    for (int i = 0; i < start; i++) {
      LOGE(" ");
    }
    int end = message->GetEndColumn();
    for (int i = start; i < end; i++) {
      LOGE("^");
    }
    LOGE("\n");
    String::Utf8Value stack_trace(try_catch->StackTrace());
    if (stack_trace.length() > 0) {
      const char* stack_trace_string = ToCString(stack_trace);
      LOGE("%s\n", stack_trace_string);
    }
  }
}

static Handle<String> ReadFile(Isolate* isolate, const char* name) {
  FILE* file = fopen(name, "rb");
  if (file == NULL) {
	LOGI("open file failed: %s %d\n", name, errno);
  	return NanNew<String>("");
  }

  fseek(file, 0, SEEK_END);
  int size = ftell(file);
  rewind(file);

  char* chars = new char[size + 1];
  chars[size] = '\0';
  for (int i = 0; i < size;) {
    int read = static_cast<int>(fread(&chars[i], 1, size - i, file));
    i += read;
  }
  fclose(file);
  Handle<String> result =
      String::NewFromUtf8(isolate, chars, String::kNormalString, size);
  delete[] chars;
  return result;
}

static bool ExecuteString(Isolate* isolate, Handle<String> source,
				Handle<Value> name, bool print_result,  bool report_exceptions) {
  HandleScope handle_scope(isolate);
  TryCatch try_catch;
  ScriptOrigin origin(name);
  Handle<Script> script = Script::Compile(source, &origin);
  if (script.IsEmpty()) {
    // Print errors that happened during compilation.
    if (report_exceptions)
      ReportException(isolate, &try_catch);
    return false;
  } else {
    Handle<Value> result = script->Run();
    if (result.IsEmpty()) {
      assert(try_catch.HasCaught());
      // Print errors that happened during execution.
      if (report_exceptions)
        ReportException(isolate, &try_catch);
      return false;
    } else {
      assert(!try_catch.HasCaught());
      if (print_result && !result->IsUndefined()) {
        // If all went well and the result wasn't undefined then print
        // the returned value.
        String::Utf8Value str(result);
        const char* cstr = ToCString(str);
        LOGI("%s\n", cstr);
      }
      return true;
    }
  }
}


void Print(const FunctionCallbackInfo<Value>& args) {
  bool first = true;
  for (int i = 0; i < args.Length(); i++) {
    HandleScope handle_scope(args.GetIsolate());
    if (first) {
      first = false;
    } else {
      CONSOLE_LOG(" ");
    }
    String::Utf8Value str(args[i]);
    const char* cstr = ToCString(str);
    CONSOLE_LOG("%s", cstr);
  }
  CONSOLE_LOG("\n");
}

void LoadFile(const char* fileName) {
	NanScope();
	Isolate* isolate = Isolate::GetCurrent();

    if (fileName == NULL) {
      isolate->ThrowException(v8::String::NewFromUtf8(isolate, "Error loading file"));
      return;
    }

    v8::Handle<v8::String> source = ReadFile(isolate, fileName);
    if (source.IsEmpty()) {
      isolate->ThrowException(
           v8::String::NewFromUtf8(isolate, "Error loading file"));
      return;
    }

	int len = strlen(fileName);
	if(strcasecmp(fileName+len-5, ".html") == 0 || strcasecmp(fileName+len-4, ".htm") == 0) {
		v8::String::Utf8Value s(source);
		string str = extractScriptInHTML(*s);
		source = NanNew(str.c_str());
		LOGI("load html file %s:\n%s\n", fileName, str.c_str());
	}

    if (!ExecuteString(isolate,
                       source,
                       v8::String::NewFromUtf8(isolate, fileName),
                       true,
                       true)) {
      isolate->ThrowException(
          v8::String::NewFromUtf8(isolate, "Error executing file"));
      return;
    }
}

void Load(const v8::FunctionCallbackInfo<v8::Value>& args) {
  Isolate* isolate = args.GetIsolate();
  for (int i = 0; i < args.Length(); i++) {
    v8::HandleScope handle_scope(isolate);
    v8::String::Utf8Value file(args[i]);
  	
  	LoadFile(*file);
  }
}

Handle<Context> CreateDefaultContext(Isolate* isolate) {
  // Create a template for the global object.
  Handle<ObjectTemplate> global = ObjectTemplate::New(isolate);
  // Bind the global 'print' function to the C++ Print callback.
  global->Set(String::NewFromUtf8(isolate, "print"), FunctionTemplate::New(isolate, Print));
  global->Set(String::NewFromUtf8(isolate, "require"), FunctionTemplate::New(isolate, Load));

  Handle<Context> context = Context::New(isolate, NULL, global);
  
  return context;
}

int loadStartup(Isolate* isolate) {
	NanScope();

	string fileName = Config::toSysAbsPath("scripts/startup.js");
	Handle<String> source = ReadFile(isolate, fileName.c_str());

	if(source->Length() < 1) {
		source = NanNew("function tick(t, dt) {\n print(\"dummy tick.\");\n}");		
	}

	int result = !ExecuteString(isolate, source, NanNew<String>(fileName.c_str()), false, true);

	return result;
}


///////////////////////////////////////////
V8Wrapper::V8Wrapper() {

}

V8Wrapper::~V8Wrapper() {
}
	
void V8Wrapper::loadApp(const char* appIndex) {
	if(appIndex) {
		Config::setAppIndex(appIndex);
	};
	
	chdir(Config::appRoot.c_str());
	LoadFile(Config::appIndex.c_str());
	LOGI("V8Wrapper::loadApp: %s\n", Config::appIndex.c_str());

	return;
}

void V8Wrapper::init(int argc, char* argv[]) {
	LOGI("V8Wrapper::init:%d\n", argc);
	V8::InitializeICU();
	Platform* platform = platform::CreateDefaultPlatform();
	V8::InitializePlatform(platform);
	V8::Initialize();
	V8::SetFlagsFromCommandLine(&argc, argv, true);
	V8::SetArrayBufferAllocator(&array_buffer_allocator);
	Isolate* isolate = Isolate::New();
	
	isolate->Enter();
    HandleScope handle_scope(isolate);
	V8Wrapper::sContext = CreateDefaultContext(isolate);
	V8Wrapper::sContext->Enter();

    if (V8Wrapper::sContext.IsEmpty()) {
      LOGE("Error creating context\n");
      return;
    }

	V8Wrapper::sPlatform = platform;
	V8Wrapper::sIsolate = isolate;

	nativeInitBinding(V8Wrapper::sContext->Global());
	CanvasRenderingContext2d::init();

    loadStartup(isolate);
	Handle<Value> tickVal = V8Wrapper::sContext->Global()->Get(NanNew("tick"));
	if (!tickVal->IsFunction()) {
		LOGI("Error: Script does not declare 'tick' global function.\n");
		return;
	}
	V8Wrapper::sTickFunc = new NanCallback(tickVal.As<Function>());

	Handle<Value> dispatchEventVal = V8Wrapper::sContext->Global()->Get(NanNew("dispatchEvent"));
	if (!dispatchEventVal->IsFunction()) {
		LOGI("Error: Script does not declare 'dispatchEvent' global function.\n");
		return;
	}
	V8Wrapper::sDispatchEventFunc = new NanCallback(dispatchEventVal.As<Function>());

	LOGI("V8Wrapper::init done\n");
}

void V8Wrapper::deinit() {
	V8::Dispose();
	V8::ShutdownPlatform();
	delete V8Wrapper::sPlatform;
	V8Wrapper::sPlatform = NULL;
	CanvasRenderingContext2d::deinit();
}

void V8Wrapper::resize(int w, int h) {
	CanvasRenderingContext2d::resize(w, h);
}

void V8Wrapper::tick(double t, double dt) {
	Isolate* isolate = V8Wrapper::sIsolate;
    Isolate::Scope isolate_scope(isolate);
	
	NanScope();

	TryCatch try_catch;
	Handle<Value> _argv[2];
	_argv[0] = Number::New(isolate, t);
	_argv[1] = Number::New(isolate, dt);

	CanvasRenderingContext2d::beginPaint();
	Handle<Value> result = V8Wrapper::sTickFunc->Call(2, _argv);
	CanvasRenderingContext2d::endPaint();
	
	if (try_catch.HasCaught()) {
		ReportException(isolate, &try_catch);
		return;
	}
}
	
void V8Wrapper::dispatchEvent(Handle<Object> event) {
	NanScope();
	Handle<Value> _argv[1] = {event};

	V8Wrapper::sDispatchEventFunc->Call(1, _argv);
}

void V8Wrapper::dispatchPointerEvent(int action, int button, int x, int y) {
	NanScope();
	vector<Touch> touchs;
	touchs.push_back(Touch(x, y));

	V8Wrapper::dispatchTouchEvent(action, button, touchs);
}

void V8Wrapper::dispatchTouchEvent(int action, int button, vector<Touch> touchs) {
	Isolate* isolate = V8Wrapper::sIsolate;
    Isolate::Scope isolate_scope(isolate);

	NanScope();
	int n = touchs.size();
	Handle<Object> obj = NanNew<Object>();
	Handle<Array> targetTouches = NanNew<Array>(n);

	Handle<Object> touchObj;
	for(int i = 0; i < n; i++) {
		Touch touch = touchs[i];
		touchObj = NanNew<Object>();
		touchObj->Set(NanNew("identifier"), NanNew<Integer>(0));
		touchObj->Set(NanNew("x"), NanNew<Integer>(touch.x));
		touchObj->Set(NanNew("pageX"), NanNew<Integer>(touch.x));
		touchObj->Set(NanNew("y"), NanNew<Integer>(touch.y));
		touchObj->Set(NanNew("pageY"), NanNew<Integer>(touch.y));
		targetTouches->Set(NanNew<Integer>(i), touchObj);
	//	LOGI("V8Wrapper::dispatchTouchEvent: action=%d x=%d y=%d\n", action, touch.x, touch.y);
	}

	obj->Set(NanNew("targetTouches"), targetTouches);
	if(action == 1) {
		obj->Set(NanNew("name"), NanNew("touchstart"));
	}
	else if(action == 0) {
		obj->Set(NanNew("name"), NanNew("touchend"));
	}
	else if(action == 3) {
		obj->Set(NanNew("name"), NanNew("touchmove"));
	}

	obj->Set(NanNew("action"), NanNew<Number>(action));
	obj->Set(NanNew("button"), NanNew<Number>(button));

	V8Wrapper::dispatchEvent(obj);
}

void V8Wrapper::dispatchKeyEvent(int action, int code, int mods, int scancode) {
	Isolate* isolate = V8Wrapper::sIsolate;
    Isolate::Scope isolate_scope(isolate);
	
	NanScope();
	Handle<Object> obj = NanNew<Object>();

	if(action == 1) {
		obj->Set(NanNew("name"), NanNew("keydown"));
	}
	else if(action == 0) {
		obj->Set(NanNew("name"), NanNew("keyup"));
	}
	else if(action == 2) {
		obj->Set(NanNew("name"), NanNew("keyrepeat"));
	}
	else {
		obj->Set(NanNew("name"), NanNew("key"));
	}

	obj->Set(NanNew("action"), NanNew<Number>(action));
	obj->Set(NanNew("code"), NanNew<Number>(code));
	obj->Set(NanNew("mods"), NanNew<Number>(mods));
	obj->Set(NanNew("scancode"), NanNew<Number>(scancode));

	V8Wrapper::dispatchEvent(obj);
}

Isolate* V8Wrapper::sIsolate = NULL;
Platform* V8Wrapper::sPlatform = NULL;
NanCallback* V8Wrapper::sTickFunc;
Handle<Context> V8Wrapper::sContext;
NanCallback* V8Wrapper::sDispatchEventFunc;


