// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

#include "base/bind.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/test/fuzzed_data_provider.h"
#include "base/test/scoped_task_environment.h"
#include "net/base/address_family.h"
#include "net/base/address_list.h"
#include "net/base/net_errors.h"
#include "net/base/request_priority.h"
#include "net/dns/fuzzed_context_host_resolver.h"
#include "net/dns/host_resolver.h"
#include "net/log/net_log_with_source.h"
#include "net/log/test_net_log.h"
#include "net/net_buildflags.h"

namespace {

const char* kHostNames[] = {"foo", "foo.com",   "a.foo.com",
                            "bar", "localhost", "localhost6"};

class DnsRequest {
 public:
  DnsRequest(net::HostResolver* host_resolver,
             base::FuzzedDataProvider* data_provider,
             std::vector<std::unique_ptr<DnsRequest>>* dns_requests)
      : host_resolver_(host_resolver),
        data_provider_(data_provider),
        dns_requests_(dns_requests) {}

  ~DnsRequest() = default;

  // Creates and starts a DNS request using fuzzed parameters. If the request
  // doesn't complete synchronously, adds it to |dns_requests|.
  static void CreateRequest(
      net::HostResolver* host_resolver,
      base::FuzzedDataProvider* data_provider,
      std::vector<std::unique_ptr<DnsRequest>>* dns_requests) {
    std::unique_ptr<DnsRequest> dns_request(
        new DnsRequest(host_resolver, data_provider, dns_requests));

    if (dns_request->Start() == net::ERR_IO_PENDING)
      dns_requests->push_back(std::move(dns_request));
  }

  // If |dns_requests| is non-empty, waits for a randomly chosen one of the
  // requests to complete and removes it from |dns_requests|.
  static void WaitForRequestComplete(
      base::FuzzedDataProvider* data_provider,
      std::vector<std::unique_ptr<DnsRequest>>* dns_requests) {
    if (dns_requests->empty())
      return;
    uint32_t index = data_provider->ConsumeIntegralInRange<uint32_t>(
        0, dns_requests->size() - 1);

    // Remove the request from the list before waiting on it - this prevents one
    // of the other callbacks from deleting the callback being waited on.
    std::unique_ptr<DnsRequest> request = std::move((*dns_requests)[index]);
    dns_requests->erase(dns_requests->begin() + index);
    request->WaitUntilDone();
  }

  // If |dns_requests| is non-empty, attempts to cancel a randomly chosen one of
  // them and removes it from |dns_requests|. If the one it picks is already
  // complete, just removes it from the list.
  static void CancelRequest(
      net::HostResolver* host_resolver,
      base::FuzzedDataProvider* data_provider,
      std::vector<std::unique_ptr<DnsRequest>>* dns_requests) {
    if (dns_requests->empty())
      return;
    uint32_t index = data_provider->ConsumeIntegralInRange<uint32_t>(
        0, dns_requests->size() - 1);
    auto request = dns_requests->begin() + index;
    (*request)->Cancel();
    dns_requests->erase(request);
  }

 private:
  void OnCallback(int result) {
    CHECK_NE(net::ERR_IO_PENDING, result);

    request_.reset();

    // Remove |this| from |dns_requests| and take ownership of it, if it wasn't
    // already removed from the vector. It may have been removed if this is in a
    // WaitForRequest call, in which case, do nothing.
    std::unique_ptr<DnsRequest> self;
    for (auto request = dns_requests_->begin(); request != dns_requests_->end();
         ++request) {
      if (request->get() != this)
        continue;
      self = std::move(*request);
      dns_requests_->erase(request);
      break;
    }

    while (true) {
      bool done = false;
      switch (data_provider_->ConsumeIntegralInRange(0, 2)) {
        case 0:
          // Quit on 0, or when no data is left.
          done = true;
          break;
        case 1:
          CreateRequest(host_resolver_, data_provider_, dns_requests_);
          break;
        case 2:
          CancelRequest(host_resolver_, data_provider_, dns_requests_);
          break;
      }

      if (done)
        break;
    }

    if (run_loop_)
      run_loop_->Quit();
  }

  // Starts the DNS request, using a fuzzed set of parameters.
  int Start() {
    net::HostResolver::ResolveHostParameters parameters;
    parameters.dns_query_type =
        data_provider_->PickValueInArray(net::kDnsQueryTypes);
    parameters.initial_priority = static_cast<net::RequestPriority>(
        data_provider_->ConsumeIntegralInRange<int32_t>(net::MINIMUM_PRIORITY,
                                                        net::MAXIMUM_PRIORITY));

    parameters.source =
        data_provider_->PickValueInArray(net::kHostResolverSources);
#if !BUILDFLAG(ENABLE_MDNS)
    while (parameters.source == net::HostResolverSource::MULTICAST_DNS) {
      parameters.source =
          data_provider_->PickValueInArray(net::kHostResolverSources);
    }
#endif  // !BUILDFLAG(ENABLE_MDNS)

    parameters.cache_usage =
        data_provider_->ConsumeBool()
            ? net::HostResolver::ResolveHostParameters::CacheUsage::ALLOWED
            : net::HostResolver::ResolveHostParameters::CacheUsage::DISALLOWED;
    parameters.include_canonical_name = data_provider_->ConsumeBool();

    const char* hostname = data_provider_->PickValueInArray(kHostNames);
    request_ = host_resolver_->CreateRequest(
        net::HostPortPair(hostname, 80), net::NetLogWithSource(), parameters);
    int rv = request_->Start(
        base::BindOnce(&DnsRequest::OnCallback, base::Unretained(this)));
    if (rv != net::ERR_IO_PENDING)
      request_.reset();
    return rv;
  }

  // Waits until the request is done, if it isn't done already.
  void WaitUntilDone() {
    CHECK(!run_loop_);
    if (request_) {
      run_loop_.reset(new base::RunLoop());
      run_loop_->Run();
      run_loop_.reset();
    }
  }

  // Cancel the request, if not already completed. Otherwise, does nothing.
  void Cancel() { request_.reset(); }

  net::HostResolver* host_resolver_;
  base::FuzzedDataProvider* data_provider_;
  std::vector<std::unique_ptr<DnsRequest>>* dns_requests_;

  // Non-null only while running.
  std::unique_ptr<net::HostResolver::ResolveHostRequest> request_;
  net::AddressList address_list_;

  std::unique_ptr<base::RunLoop> run_loop_;

  DISALLOW_COPY_AND_ASSIGN(DnsRequest);
};

}  // namespace

// Fuzzer for HostResolverImpl. Fuzzes using both the system resolver and
// built-in DNS client paths.
//
// TODO(mmenke): Add coverage for things this does not cover:
//     * Out of order completion, particularly for the platform resolver path.
//     * Simulate network changes, including both enabling and disabling the
//     async resolver while lookups are active as a result of the change.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  {
    base::FuzzedDataProvider data_provider(data, size);
    net::TestNetLog net_log;

    net::HostResolver::Options options;
    options.max_concurrent_resolves =
        data_provider.ConsumeIntegralInRange(1, 8);
    options.enable_caching = data_provider.ConsumeBool();
    net::FuzzedContextHostResolver host_resolver(options, &net_log,
                                                 &data_provider);
    host_resolver.SetDnsClientEnabled(data_provider.ConsumeBool());

    std::vector<std::unique_ptr<DnsRequest>> dns_requests;
    bool done = false;
    while (!done) {
      switch (data_provider.ConsumeIntegralInRange(0, 3)) {
        case 0:
          // Quit on 0, or when no data is left.
          done = true;
          break;
        case 1:
          DnsRequest::CreateRequest(&host_resolver, &data_provider,
                                    &dns_requests);
          break;
        case 2:
          DnsRequest::WaitForRequestComplete(&data_provider, &dns_requests);
          break;
        case 3:
          DnsRequest::CancelRequest(&host_resolver, &data_provider,
                                    &dns_requests);
          break;
      }
    }
  }

  // Clean up any pending tasks, after deleting everything.
  base::RunLoop().RunUntilIdle();

  return 0;
}
