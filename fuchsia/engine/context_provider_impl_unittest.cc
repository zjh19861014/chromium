// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fuchsia/engine/context_provider_impl.h"

#include <fuchsia/web/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/cpp/binding.h>
#include <zircon/processargs.h>
#include <zircon/types.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "base/base_paths_fuchsia.h"
#include "base/base_switches.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/fuchsia/default_job.h"
#include "base/fuchsia/file_utils.h"
#include "base/fuchsia/fuchsia_logging.h"
#include "base/fuchsia/service_directory.h"
#include "base/message_loop/message_loop.h"
#include "base/path_service.h"
#include "base/test/multiprocess_test.h"
#include "base/test/test_timeouts.h"
#include "fuchsia/engine/common.h"
#include "fuchsia/engine/fake_context.h"
#include "fuchsia/engine/legacy_context_provider_bridge.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"

namespace {

constexpr char kTestDataFileIn[] = "DataFileIn";
constexpr char kTestDataFileOut[] = "DataFileOut";
constexpr char kUrl[] = "chrome://:emorhc";
constexpr char kTitle[] = "Palindrome";

MULTIPROCESS_TEST_MAIN(SpawnContextServer) {
  base::MessageLoopForIO message_loop;

  base::FilePath data_dir;
  CHECK(base::PathService::Get(base::DIR_APP_DATA, &data_dir));
  if (!data_dir.empty()) {
    if (base::PathExists(data_dir.AppendASCII(kTestDataFileIn))) {
      auto out_file = data_dir.AppendASCII(kTestDataFileOut);
      EXPECT_EQ(base::WriteFile(out_file, nullptr, 0), 0);
    }
  }

  fidl::InterfaceRequest<fuchsia::web::Context> fuchsia_context(
      zx::channel(zx_take_startup_handle(kContextRequestHandleId)));
  CHECK(fuchsia_context);

  FakeContext context;
  fidl::Binding<fuchsia::web::Context> context_binding(
      &context, std::move(fuchsia_context));

  // When a Frame's NavigationEventListener is bound, immediately broadcast a
  // navigation event to its listeners.
  context.set_on_create_frame_callback(
      base::BindRepeating([](FakeFrame* frame) {
        frame->set_on_set_listener_callback(base::BindOnce(
            [](FakeFrame* frame) {
              fuchsia::web::NavigationState state;
              state.set_url(kUrl);
              state.set_title(kTitle);
              frame->listener()->OnNavigationStateChanged(std::move(state),
                                                          []() {});
            },
            frame));
      }));

  // Quit the process when the context is destroyed.
  base::RunLoop run_loop;
  context_binding.set_error_handler([&run_loop](zx_status_t status) {
    EXPECT_EQ(status, ZX_ERR_PEER_CLOSED);
    run_loop.Quit();
  });
  run_loop.Run();

  return 0;
}

base::Process LaunchFakeContextProcess(const base::CommandLine& command_line,
                                       const base::LaunchOptions& options) {
  base::LaunchOptions options_with_tmp = options;
  options_with_tmp.paths_to_clone.push_back(base::FilePath("/tmp"));
  return base::SpawnMultiProcessTestChild("SpawnContextServer", command_line,
                                          options_with_tmp);
}

}  // namespace

class ContextProviderImplTest : public base::MultiProcessTest {
 public:
  ContextProviderImplTest()
      : context_provider_(std::make_unique<ContextProviderImpl>()) {
    fuchsia::web::ContextProviderPtr fuchsia_context_provider;
    legacy_binding_ =
        std::make_unique<fidl::Binding<fuchsia::web::ContextProvider>>(
            context_provider_.get(), fuchsia_context_provider.NewRequest());
    provider_ = std::make_unique<LegacyContextProviderBridge>(
        std::move(fuchsia_context_provider));

    context_provider_->SetLaunchCallbackForTest(
        base::BindRepeating(&LaunchFakeContextProcess));
    bindings_.AddBinding(provider_.get(), provider_ptr_.NewRequest());
  }

  ~ContextProviderImplTest() override {
    provider_ptr_.Unbind();
    base::RunLoop().RunUntilIdle();
  }

  // Check if a Context is responsive by creating a Frame from it and then
  // listening for an event.
  void CheckContextResponsive(
      fidl::InterfacePtr<chromium::web::Context>* context) {
    // Call a Context method and wait for it to invoke an observer call.
    base::RunLoop run_loop;
    context->set_error_handler([&run_loop](zx_status_t status) {
      ZX_LOG(ERROR, status) << " Context lost.";
      ADD_FAILURE();
      run_loop.Quit();
    });

    chromium::web::FramePtr frame_ptr;
    frame_ptr.set_error_handler([&run_loop](zx_status_t status) {
      ZX_LOG(ERROR, status) << " Frame lost.";
      ADD_FAILURE();
      run_loop.Quit();
    });
    (*context)->CreateFrame(frame_ptr.NewRequest());

    // Create a Frame and expect to see a navigation event.
    CapturingNavigationEventObserver change_observer(run_loop.QuitClosure());
    fidl::Binding<chromium::web::NavigationEventObserver>
        change_observer_binding(&change_observer);
    frame_ptr->SetNavigationEventObserver(change_observer_binding.NewBinding());
    run_loop.Run();

    EXPECT_EQ(change_observer.captured_event().url, kUrl);
    EXPECT_EQ(change_observer.captured_event().title, kTitle);
  }

  chromium::web::CreateContextParams BuildCreateContextParams() {
    fidl::InterfaceHandle<fuchsia::io::Directory> directory;
    zx_status_t result =
        fdio_service_connect(base::fuchsia::kServiceDirectoryPath,
                             directory.NewRequest().TakeChannel().release());
    ZX_CHECK(result == ZX_OK, result) << "Failed to open /svc";

    chromium::web::CreateContextParams output;
    output.set_service_directory(std::move(directory));
    return output;
  }

  // Checks that the Context channel was dropped.
  void CheckContextUnresponsive(
      fidl::InterfacePtr<chromium::web::Context>* context) {
    base::RunLoop run_loop;
    context->set_error_handler([&run_loop](zx_status_t status) {
      EXPECT_EQ(status, ZX_ERR_PEER_CLOSED);
      run_loop.Quit();
    });

    chromium::web::FramePtr frame;
    (*context)->CreateFrame(frame.NewRequest());

    // The error handler should be called here.
    run_loop.Run();
  }

 protected:
  base::MessageLoopForIO message_loop_;
  std::unique_ptr<LegacyContextProviderBridge> provider_;
  std::unique_ptr<fidl::Binding<fuchsia::web::ContextProvider>> legacy_binding_;
  chromium::web::ContextProviderPtr provider_ptr_;
  fidl::BindingSet<chromium::web::ContextProvider> bindings_;

 private:
  struct CapturingNavigationEventObserver
      : public chromium::web::NavigationEventObserver {
   public:
    explicit CapturingNavigationEventObserver(base::OnceClosure on_change_cb)
        : on_change_cb_(std::move(on_change_cb)) {}
    ~CapturingNavigationEventObserver() override = default;

    void OnNavigationStateChanged(
        chromium::web::NavigationEvent change,
        OnNavigationStateChangedCallback callback) override {
      captured_event_ = std::move(change);
      std::move(on_change_cb_).Run();
    }

    chromium::web::NavigationEvent captured_event() { return captured_event_; }

   private:
    base::OnceClosure on_change_cb_;
    chromium::web::NavigationEvent captured_event_;
  };

  std::unique_ptr<ContextProviderImpl> context_provider_;

  DISALLOW_COPY_AND_ASSIGN(ContextProviderImplTest);
};

TEST_F(ContextProviderImplTest, LaunchContext) {
  // Connect to a new context process.
  fidl::InterfacePtr<chromium::web::Context> context;
  chromium::web::CreateContextParams create_params = BuildCreateContextParams();
  provider_ptr_->Create(std::move(create_params), context.NewRequest());
  CheckContextResponsive(&context);
}

TEST_F(ContextProviderImplTest, MultipleConcurrentClients) {
  // Bind a Provider connection, and create a Context from it.
  chromium::web::ContextProviderPtr provider_1_ptr;
  bindings_.AddBinding(provider_.get(), provider_1_ptr.NewRequest());
  chromium::web::ContextPtr context_1;
  provider_1_ptr->Create(BuildCreateContextParams(), context_1.NewRequest());

  // Do the same on another Provider connection.
  chromium::web::ContextProviderPtr provider_2_ptr;
  bindings_.AddBinding(provider_.get(), provider_2_ptr.NewRequest());
  chromium::web::ContextPtr context_2;
  provider_2_ptr->Create(BuildCreateContextParams(), context_2.NewRequest());

  CheckContextResponsive(&context_1);
  CheckContextResponsive(&context_2);

  // Ensure that the initial ContextProvider connection is still usable, by
  // creating and verifying another Context from it.
  chromium::web::ContextPtr context_3;
  provider_2_ptr->Create(BuildCreateContextParams(), context_3.NewRequest());
  CheckContextResponsive(&context_3);
}

TEST_F(ContextProviderImplTest, WithProfileDir) {
  base::ScopedTempDir profile_temp_dir;

  // Connect to a new context process.
  fidl::InterfacePtr<chromium::web::Context> context;
  chromium::web::CreateContextParams create_params = BuildCreateContextParams();

  // Setup data dir.
  EXPECT_TRUE(profile_temp_dir.CreateUniqueTempDir());
  ASSERT_EQ(
      base::WriteFile(profile_temp_dir.GetPath().AppendASCII(kTestDataFileIn),
                      nullptr, 0),
      0);

  // Pass a handle data dir to the context.
  create_params.set_data_directory(
      base::fuchsia::OpenDirectory(profile_temp_dir.GetPath()));

  provider_ptr_->Create(std::move(create_params), context.NewRequest());

  CheckContextResponsive(&context);

  // Verify that the context process can write to the data dir.
  EXPECT_TRUE(base::PathExists(
      profile_temp_dir.GetPath().AppendASCII(kTestDataFileOut)));
}

TEST_F(ContextProviderImplTest, FailsDataDirectoryIsFile) {
  base::FilePath temp_file_path;

  // Connect to a new context process.
  fidl::InterfacePtr<chromium::web::Context> context;
  chromium::web::CreateContextParams create_params = BuildCreateContextParams();

  // Pass in a handle to a file instead of a directory.
  CHECK(base::CreateTemporaryFile(&temp_file_path));
  create_params.set_data_directory(
      base::fuchsia::OpenDirectory(temp_file_path));

  provider_ptr_->Create(std::move(create_params), context.NewRequest());

  CheckContextUnresponsive(&context);
}

static bool WaitUntilJobIsEmpty(zx::unowned_job job, zx::duration timeout) {
  zx_signals_t observed = 0;
  zx_status_t status =
      job->wait_one(ZX_JOB_NO_JOBS, zx::deadline_after(timeout), &observed);
  ZX_CHECK(status == ZX_OK || status == ZX_ERR_TIMED_OUT, status);
  return observed & ZX_JOB_NO_JOBS;
}

// Regression test for https://crbug.com/927403 (Job leak per-Context).
TEST_F(ContextProviderImplTest, CleansUpContextJobs) {
  // Replace the default job with one that is guaranteed to be empty.
  zx::job job;
  ASSERT_EQ(base::GetDefaultJob()->duplicate(ZX_RIGHT_SAME_RIGHTS, &job),
            ZX_OK);
  base::ScopedDefaultJobForTest empty_default_job(std::move(job));

  // Bind to the ContextProvider.
  chromium::web::ContextProviderPtr provider;
  bindings_.AddBinding(provider_.get(), provider.NewRequest());

  // Verify that our current default job is still empty.
  ASSERT_TRUE(WaitUntilJobIsEmpty(base::GetDefaultJob(), zx::duration()));

  // Create a Context and verify that it is functional.
  chromium::web::ContextPtr context;
  provider->Create(BuildCreateContextParams(), context.NewRequest());
  CheckContextResponsive(&context);

  // Verify that there is at least one job under our default job.
  ASSERT_FALSE(WaitUntilJobIsEmpty(base::GetDefaultJob(), zx::duration()));

  // Detach from the Context and ContextProvider, and spin the loop to allow
  // those to be handled.
  context.Unbind();
  provider.Unbind();
  base::RunLoop().RunUntilIdle();

  // Wait until the default job signals that it no longer contains any child
  // jobs; this should occur shortly after the Context process terminates.
  EXPECT_TRUE(WaitUntilJobIsEmpty(
      base::GetDefaultJob(),
      zx::duration(TestTimeouts::action_timeout().InNanoseconds())));
}
