#include <libplatform/libplatform.h>
#include <spdlog/spdlog.h>
#include <v8.h>

#include <boost/asio.hpp>
#include <future>
#include <mutex>
#include <thread>

#include "js_engine.hpp"

namespace ytdlpp::scripting {

struct JsEngine::Impl {
	v8::Isolate *isolate = nullptr;
	v8::Global<v8::Context> context;
	v8::Isolate::CreateParams create_params;

	// Worker logic
	std::thread worker;
	boost::asio::io_context ioc;
	std::unique_ptr<boost::asio::executor_work_guard<
		boost::asio::io_context::executor_type>>
		work_guard;
	std::atomic<bool> shutdown_flag{false};

	Impl() {
		work_guard = std::make_unique<boost::asio::executor_work_guard<
			boost::asio::io_context::executor_type>>(ioc.get_executor());

		// Use promise to synchronize V8 initialization
		std::promise<void> init_promise;
		auto init_future = init_promise.get_future();

		// Start thread
		worker = std::thread(
			[this, init_promise = std::move(init_promise)]() mutable {
				this->InitializeV8OnThread();
				init_promise.set_value();  // Signal that V8 is ready
				this->ioc.run();
				this->CleanupV8OnThread();
			});

		// Wait for V8 to be fully initialized before returning
		init_future.wait();
	}

	~Impl() { shutdown(); }

	void shutdown() {
		if (shutdown_flag.exchange(true)) { return; }  // Already shut down

		// Terminate any running V8 scripts first
		if (isolate) { isolate->TerminateExecution(); }

		// Release work guard to allow ioc.run() to exit
		work_guard->reset();

		// Stop io_context - this should cause ioc.run() to return
		ioc.stop();

		// Attempt to join the worker thread. The worker should exit quickly
		// since ioc.run() has been stopped. However, V8's Isolate::Dispose()
		// in CleanupV8OnThread() can sometimes deadlock on Windows due to
		// internal GC sweeper thread issues (see Chromium bug tracker).
		//
		// Best practice says prefer join() over detach(). We try join first
		// with a short wait, only falling back to detach if it hangs.
		if (worker.joinable()) {
			// Use a helper thread to implement a timeout for join
			// IMPORTANT: joined must be heap-allocated because if we detach
			// the joiner thread, it can outlive this function and access
			// 'joined'
			auto joined = std::make_shared<std::atomic<bool>>(false);
			std::thread joiner([this, joined]() {
				worker.join();
				joined->store(true);
			});

			// Give the worker up to 500ms to clean up and exit
			auto deadline = std::chrono::steady_clock::now() +
							std::chrono::milliseconds(500);
			while (!joined->load() &&
				   std::chrono::steady_clock::now() < deadline) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}

			if (joined->load()) {
				joiner.join();
			} else {
				// V8 cleanup can hang on Windows - detach to avoid blocking
				// shutdown (known Chromium issue with GC sweeper threads)
				joiner.detach();
			}
		}
	}

	void InitializeV8OnThread() {
		// Platform init (global)
		static std::once_flag init_flag;
		static std::unique_ptr<v8::Platform> g_platform;
		std::call_once(init_flag, []() {
			g_platform = v8::platform::NewDefaultPlatform();
			v8::V8::InitializePlatform(g_platform.get());
			v8::V8::Initialize();
		});

		create_params.array_buffer_allocator =
			v8::ArrayBuffer::Allocator::NewDefaultAllocator();
		isolate = v8::Isolate::New(create_params);

		v8::Isolate::Scope isolate_scope(isolate);
		v8::HandleScope handle_scope(isolate);
		v8::Local<v8::Context> ctx = v8::Context::New(isolate);

		if (ctx.IsEmpty()) {
			spdlog::error("JsEngine: Failed to create Context");
			return;
		}

		context.Reset(isolate, ctx);
	}

	void CleanupV8OnThread() {
		if (isolate) {
			context.Reset();
			isolate->Dispose();
			delete create_params.array_buffer_allocator;
		}
	}

	// Helper to run tasks on worker (synchronous)
	template <typename Func>
	auto RunOnWorker(Func &&func) {
		using ResultType =
			std::invoke_result_t<Func, v8::Isolate *, v8::Local<v8::Context>>;

		std::promise<ResultType> promise;
		auto future = promise.get_future();

		boost::asio::post(ioc, [this, p = std::move(promise),
								f = std::forward<Func>(func)]() mutable {
			if (!isolate || shutdown_flag.load()) {
				if constexpr (std::is_void_v<ResultType>)
					p.set_value();
				else
					p.set_exception(std::make_exception_ptr(std::runtime_error(
						"V8 not initialized or shutting down")));
				return;
			}

			v8::Isolate::Scope isolate_scope(isolate);
			v8::HandleScope handle_scope(isolate);
			v8::Local<v8::Context> ctx = context.Get(isolate);
			v8::Context::Scope context_scope(ctx);

			try {
				if constexpr (std::is_void_v<ResultType>) {
					f(isolate, ctx);
					p.set_value();
				} else {
					p.set_value(f(isolate, ctx));
				}
			} catch (...) { p.set_exception(std::current_exception()); }
		});

		return future.get();
	}

	// Helper to run tasks on worker (async)
	template <typename Func, typename Handler>
	void RunOnWorkerAsync(Func &&func, Handler &&handler) {
		boost::asio::post(ioc, [this, f = std::forward<Func>(func),
								h = std::forward<Handler>(handler)]() mutable {
			if (!isolate || shutdown_flag.load()) {
				h(outcome::failure(
					std::make_error_code(std::errc::state_not_recoverable)));
				return;
			}

			v8::Isolate::Scope isolate_scope(isolate);
			v8::HandleScope handle_scope(isolate);
			v8::Local<v8::Context> ctx = context.Get(isolate);
			v8::Context::Scope context_scope(ctx);

			try {
				auto res = f(isolate, ctx);
				h(res);
			} catch (...) {
				using ResType = std::invoke_result_t<Func, v8::Isolate *,
													 v8::Local<v8::Context>>;
				h(ResType(outcome::failure(
					std::make_error_code(std::errc::operation_canceled))));
			}
		});
	}
};

JsEngine::JsEngine(boost::asio::any_io_executor)
	: impl_(std::make_unique<Impl>()) {}

JsEngine::~JsEngine() = default;

void JsEngine::shutdown() {
	if (impl_) { impl_->shutdown(); }
}

Result<void> JsEngine::evaluate(const std::string &code) {
	auto task = [&](v8::Isolate *isolate,
					v8::Local<v8::Context> context) -> Result<void> {
		v8::Local<v8::String> source;
		if (!v8::String::NewFromUtf8(isolate, code.c_str()).ToLocal(&source)) {
			return std::make_error_code(std::errc::invalid_argument);
		}

		v8::Local<v8::Script> script;
		v8::TryCatch try_catch(isolate);

		if (!v8::Script::Compile(context, source).ToLocal(&script)) {
			if (try_catch.HasCaught()) {
				v8::String::Utf8Value error(isolate, try_catch.Exception());
				spdlog::error(
					"JsEngine: Compile Error: {}", *error ? *error : "Unknown");
			}
			return std::make_error_code(std::errc::invalid_argument);
		}

		v8::Local<v8::Value> result;
		if (!script->Run(context).ToLocal(&result)) {
			if (try_catch.HasCaught()) {
				v8::String::Utf8Value error(isolate, try_catch.Exception());
				spdlog::error(
					"JsEngine: Runtime Error: {}", *error ? *error : "Unknown");
			}
			return std::make_error_code(std::errc::invalid_argument);
		}
		return outcome::success();
	};
	return impl_->RunOnWorker(std::move(task));
}

Result<std::string> JsEngine::evaluate_and_get(const std::string &code) {
	auto task = [&](v8::Isolate *isolate,
					v8::Local<v8::Context> context) -> Result<std::string> {
		v8::Local<v8::String> source;
		if (!v8::String::NewFromUtf8(isolate, code.c_str()).ToLocal(&source))
			return std::make_error_code(std::errc::invalid_argument);

		v8::Local<v8::Script> script;
		v8::TryCatch try_catch(isolate);
		if (!v8::Script::Compile(context, source).ToLocal(&script)) {
			if (try_catch.HasCaught()) {
				v8::String::Utf8Value error(isolate, try_catch.Exception());
				spdlog::error("JsEngine: Compile Error (get): {}",
							  *error ? *error : "Unknown");
			}
			return std::make_error_code(std::errc::invalid_argument);
		}

		v8::Local<v8::Value> result;
		if (!script->Run(context).ToLocal(&result)) {
			if (try_catch.HasCaught()) {
				v8::String::Utf8Value error(isolate, try_catch.Exception());
				spdlog::error("JsEngine: Runtime Error (get): {}",
							  *error ? *error : "Unknown");
			}
			return std::make_error_code(std::errc::invalid_argument);
		}

		v8::String::Utf8Value utf8(isolate, result);
		return std::string(*utf8);
	};
	return impl_->RunOnWorker(std::move(task));
}

Result<std::string> JsEngine::call_function(
	const std::string &func_name, const std::vector<std::string> &args) {
	auto task = [&](v8::Isolate *isolate,
					v8::Local<v8::Context> context) -> Result<std::string> {
		v8::Local<v8::String> name;
		if (!v8::String::NewFromUtf8(isolate, func_name.c_str()).ToLocal(&name))
			return std::make_error_code(std::errc::invalid_argument);

		v8::Local<v8::Value> val;
		if (!context->Global()->Get(context, name).ToLocal(&val) ||
			!val->IsFunction()) {
			return std::make_error_code(std::errc::function_not_supported);
		}

		v8::Local<v8::Function> func = v8::Local<v8::Function>::Cast(val);
		std::vector<v8::Local<v8::Value>> argv;
		argv.reserve(args.size());
		for (const auto &arg : args) {
			v8::Local<v8::String> s;
			if (v8::String::NewFromUtf8(isolate, arg.c_str()).ToLocal(&s))
				argv.push_back(s);
		}

		v8::TryCatch try_catch(isolate);
		v8::Local<v8::Value> result;
		if (!func->Call(
					 context, context->Global(), (int)argv.size(), argv.data())
				 .ToLocal(&result)) {
			if (try_catch.HasCaught()) {
				v8::String::Utf8Value error(isolate, try_catch.Exception());
				spdlog::error(
					"JsEngine: Call Error: {}", *error ? *error : "Unknown");
			}
			return std::make_error_code(std::errc::operation_canceled);
		}

		v8::String::Utf8Value utf8(isolate, result);
		return std::string(*utf8);
	};
	return impl_->RunOnWorker(std::move(task));
}

void JsEngine::async_evaluate_impl(
	std::string code,
	boost::asio::any_completion_handler<void(Result<void>)> handler) {
	// Re-use logic from evaluate by duplicating?
	// Or extract common logic.
	// For now, duplicate/inline to avoid lambda mess
	// Actually, just calling the same logic:
	auto task = [code = std::move(code)](
					v8::Isolate *isolate,
					v8::Local<v8::Context> context) -> Result<void> {
		v8::Local<v8::String> source;
		if (!v8::String::NewFromUtf8(isolate, code.c_str()).ToLocal(&source)) {
			return std::make_error_code(std::errc::invalid_argument);
		}
		v8::Local<v8::Script> script;
		v8::TryCatch try_catch(isolate);
		if (!v8::Script::Compile(context, source).ToLocal(&script)) {
			if (try_catch.HasCaught()) {
				v8::String::Utf8Value error(isolate, try_catch.Exception());
				spdlog::error(
					"JsEngine: Compile Error: {}", *error ? *error : "Unknown");
			}
			return std::make_error_code(std::errc::invalid_argument);
		}
		v8::Local<v8::Value> result;
		if (!script->Run(context).ToLocal(&result)) {
			if (try_catch.HasCaught()) {
				v8::String::Utf8Value error(isolate, try_catch.Exception());
				spdlog::error(
					"JsEngine: Runtime Error: {}", *error ? *error : "Unknown");
			}
			return std::make_error_code(std::errc::invalid_argument);
		}
		return outcome::success();
	};
	impl_->RunOnWorkerAsync(std::move(task), std::move(handler));
}

void JsEngine::async_evaluate_and_get_impl(
	std::string code,
	boost::asio::any_completion_handler<void(Result<std::string>)> handler) {
	auto task = [code = std::move(code)](
					v8::Isolate *isolate,
					v8::Local<v8::Context> context) -> Result<std::string> {
		v8::Local<v8::String> source;
		if (!v8::String::NewFromUtf8(isolate, code.c_str()).ToLocal(&source))
			return std::make_error_code(std::errc::invalid_argument);
		v8::Local<v8::Script> script;
		v8::TryCatch try_catch(isolate);
		if (!v8::Script::Compile(context, source).ToLocal(&script)) {
			if (try_catch.HasCaught()) {
				v8::String::Utf8Value error(isolate, try_catch.Exception());
				spdlog::error("JsEngine: Compile Error (get): {}",
							  *error ? *error : "Unknown");
			}
			return std::make_error_code(std::errc::invalid_argument);
		}
		v8::Local<v8::Value> result;
		if (!script->Run(context).ToLocal(&result)) {
			if (try_catch.HasCaught()) {
				v8::String::Utf8Value error(isolate, try_catch.Exception());
				spdlog::error("JsEngine: Runtime Error (get): {}",
							  *error ? *error : "Unknown");
			}
			return std::make_error_code(std::errc::invalid_argument);
		}
		v8::String::Utf8Value utf8(isolate, result);
		return std::string(*utf8);
	};
	impl_->RunOnWorkerAsync(std::move(task), std::move(handler));
}

void JsEngine::async_call_function_impl(
	std::string func_name, std::vector<std::string> args,
	boost::asio::any_completion_handler<void(Result<std::string>)> handler) {
	auto task = [func_name = std::move(func_name), args = std::move(args)](
					v8::Isolate *isolate,
					v8::Local<v8::Context> context) -> Result<std::string> {
		v8::Local<v8::String> name;
		if (!v8::String::NewFromUtf8(isolate, func_name.c_str()).ToLocal(&name))
			return std::make_error_code(std::errc::invalid_argument);

		v8::Local<v8::Value> val;
		if (!context->Global()->Get(context, name).ToLocal(&val) ||
			!val->IsFunction()) {
			return std::make_error_code(std::errc::function_not_supported);
		}

		v8::Local<v8::Function> func = v8::Local<v8::Function>::Cast(val);
		std::vector<v8::Local<v8::Value>> argv;
		argv.reserve(args.size());
		for (const auto &arg : args) {
			v8::Local<v8::String> s;
			if (v8::String::NewFromUtf8(isolate, arg.c_str()).ToLocal(&s))
				argv.push_back(s);
		}

		v8::TryCatch try_catch(isolate);
		v8::Local<v8::Value> result;
		if (!func->Call(
					 context, context->Global(), (int)argv.size(), argv.data())
				 .ToLocal(&result)) {
			if (try_catch.HasCaught()) {
				v8::String::Utf8Value error(isolate, try_catch.Exception());
				spdlog::error(
					"JsEngine: Call Error: {}", *error ? *error : "Unknown");
			}
			return std::make_error_code(std::errc::operation_canceled);
		}

		v8::String::Utf8Value utf8(isolate, result);
		return std::string(*utf8);
	};
	impl_->RunOnWorkerAsync(std::move(task), std::move(handler));
}

}  // namespace ytdlpp::scripting
