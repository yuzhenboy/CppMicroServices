/*=============================================================================

Library: CppMicroServices

Copyright (c) The CppMicroServices developers. See the COPYRIGHT
file at the top-level directory of this distribution and at
https://github.com/saschazelzer/CppMicroServices/COPYRIGHT .

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

=============================================================================*/

#include "usFrameworkPrivate.h"

#include "usBundleContextPrivate.h"
#include "usBundleEvent.h"
#include "usFramework.h"
#include "usLog.h"


namespace us {

FrameworkPrivate::FrameworkPrivate(CoreBundleContext* fwCtx)
  : BundlePrivate(fwCtx)
{
  Logger::instance().SetLogLevel(static_cast<us::MsgType>(any_cast<int>(coreCtx->frameworkProperties.find(Constants::FRAMEWORK_LOG_LEVEL)->second)));
}

void FrameworkPrivate::DoInit()
{
  state = Bundle::STATE_STARTING;
  coreCtx->Init();
}

void FrameworkPrivate::Init()
{
  auto l = Lock();
  WaitOnOperation(*this, l, "Framework::Init", true);

  switch (static_cast<Bundle::State>(state.load()))
  {
  case Bundle::STATE_INSTALLED:
  case Bundle::STATE_RESOLVED:
    break;
  case Bundle::STATE_STARTING:
  case Bundle::STATE_ACTIVE:
    return;
  default:
    std::stringstream ss;
    ss << state;
    throw std::logic_error("INTERNAL ERROR, Illegal state, " + ss.str());
  }
  this->DoInit();
}

void FrameworkPrivate::InitSystemBundle()
{
  bundleContext.Store(std::make_shared<BundleContextPrivate>(this));

  // TODO Capabilities
  /*
      std::string sp;
      sp.append(coreCtx->frameworkProperties.getProperty(Constants::FRAMEWORK_SYSTEMCAPABILITIES));
      // Add in extra system capabilities
      std::string epc = coreCtx->frameworkProperties.getProperty(Constants::FRAMEWORK_SYSTEMCAPABILITIES_EXTRA);
      if (!epc.empty())
      {
        if (!sp.empty())
        {
          sp.append(',');
        }
        sp.append(epc);
      }
      provideCapabilityString = sp;
      */

  // TODO Wiring
  /*
      BundleGeneration* gen = new BundleGeneration(this, exportPackageString,
                                                        provideCapabilityString);
      generations.add(gen);
      gen->SetWired();
      fwWiring = new FrameworkWiringImpl(coreCtx);
      */
}

void FrameworkPrivate::UninitSystemBundle()
{
  bundleContext.Exchange(std::shared_ptr<BundleContextPrivate>())->Invalidate();
}

FrameworkEvent FrameworkPrivate::WaitForStop(const std::chrono::milliseconds& timeout)
{
  auto l = Lock();
  // Already stopped?
  if (((Bundle::STATE_INSTALLED | Bundle::STATE_RESOLVED) & state) == 0)
  {
    stopEvent = FrameworkEventInternal{ false, FrameworkEvent::ERROR, std::exception_ptr() };
    if (timeout.count() == 0)
    {
      Wait(l, [&] { return stopEvent.valid; });
    }
    else
    {
      WaitFor(l, timeout, [&] { return stopEvent.valid; });
    }
    if (!stopEvent.valid)
    {
      return FrameworkEvent(
            FrameworkEvent::WAIT_TIMEDOUT,
            MakeBundle(this->shared_from_this()),
            std::exception_ptr()
            );
    }
  }
  else if (!stopEvent.valid)
  {
    // Return this if stop or update have not been called and framework is
    // stopped.
    stopEvent = FrameworkEventInternal{
          true,
          FrameworkEvent::STOPPED,
          std::exception_ptr()};
  }
  if (shutdownThread.joinable()) shutdownThread.join();
  return FrameworkEvent(stopEvent.type, MakeBundle(this->shared_from_this()), stopEvent.excPtr);
}

void FrameworkPrivate::Shutdown(bool restart)
{
  auto l = Lock(); US_UNUSED(l);
  bool wasActive = false;
  switch (static_cast<Bundle::State>(state.load()))
  {
  case Bundle::STATE_INSTALLED:
  case Bundle::STATE_RESOLVED:
    ShutdownDone_unlocked(false);
    break;
  case Bundle::STATE_ACTIVE:
    wasActive = true;
    // Fall through
  case Bundle::STATE_STARTING:
  {
    const bool wa = wasActive;
#ifdef US_ENABLE_THREADING_SUPPORT
    if (!shutdownThread.joinable())
    {
      shutdownThread = std::thread(std::bind(&FrameworkPrivate::Shutdown0, this, restart, wa));
    }
#else
    Shutdown0(restart, wa);
#endif
    break;
  }
  case Bundle::STATE_UNINSTALLED:
  case Bundle::STATE_STOPPING:
    // Shutdown already inprogress
    break;
  }
}

void FrameworkPrivate::Shutdown0(bool restart, bool wasActive)
{
  try
  {
    {
      auto l = Lock();
      WaitOnOperation(*this, l, std::string("Framework::") + (restart ? "Update" : "Stop"), true);
      operation = OP_DEACTIVATING;
      state = Bundle::STATE_STOPPING;
    }
    coreCtx->listeners.BundleChanged(BundleEvent(BundleEvent::STOPPING, MakeBundle(this->shared_from_this())));
    if (wasActive)
    {
      StopAllBundles();
    }
    coreCtx->Uninit0();
    {
      auto l = Lock();
      coreCtx->Uninit1();
      ShutdownDone_unlocked(restart);
    }
    if (restart)
    {
      if (wasActive)
      {
        Start(0);
      }
      else
      {
        Init();
      }
    }
  }
  catch (...)
  {
    auto l = Lock();
    SystemShuttingdownDone_unlocked(
          FrameworkEventInternal{
            true,
            FrameworkEvent::ERROR,
            std::current_exception()
          }
          );
  }
}

void FrameworkPrivate::ShutdownDone_unlocked(bool restart)
{
  auto t = restart ? FrameworkEvent::STOPPED_UPDATE : FrameworkEvent::STOPPED;
  SystemShuttingdownDone_unlocked(
        FrameworkEventInternal{
          true,
          t,
          std::exception_ptr()});
}

void FrameworkPrivate::StopAllBundles()
{
  // Stop all active bundles, in reverse bundle ID order
  auto activeBundles = coreCtx->bundleRegistry.GetActiveBundles();
  for (auto iter = activeBundles.rbegin(); iter != activeBundles.rend(); ++iter)
  {
    auto b = *iter;
    try
    {
      if (((Bundle::STATE_ACTIVE | Bundle::STATE_STARTING) & b->state) != 0)
      {
        // Stop bundle without changing its autostart setting.
        b->Stop(Bundle::StopOptions::STOP_TRANSIENT);
      }
    }
    catch (...)
    {
      // $TODO
      // coreCtx->FrameworkError(b, std::current_exception());
      try { throw; } catch (const std::exception& e) { US_ERROR << e.what(); }
    }
  }

  auto allBundles = coreCtx->bundleRegistry.GetBundles();

  // Set state to INSTALLED
  for (auto b : allBundles)
  {
    if (b->id != 0)
    {
      auto l = coreCtx->resolver.Lock();
      b->SetStateInstalled(false, l);
    }
  }
}

void FrameworkPrivate::SystemShuttingdownDone_unlocked(const FrameworkEventInternal& fe)
{
  if (state != Bundle::STATE_INSTALLED)
  {
    state = Bundle::STATE_RESOLVED;
    operation = OP_IDLE;
    NotifyAll();
  }
  stopEvent = fe;
}

}
