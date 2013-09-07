/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FrameworkView.h"
#include "MetroAppShell.h"
#include "MetroWidget.h"
#include "mozilla/AutoRestore.h"
#include "MetroUtils.h"
#include "MetroApp.h"
#include "UIABridgePublic.h"
#include "KeyboardLayout.h"

// generated
#include "UIABridge.h"

using namespace mozilla;
using namespace mozilla::widget::winrt;
#ifdef ACCESSIBILITY
using namespace mozilla::a11y;
#endif
using namespace ABI::Windows::ApplicationModel;
using namespace ABI::Windows::ApplicationModel::Core;
using namespace ABI::Windows::ApplicationModel::Activation;
using namespace ABI::Windows::UI::Core;
using namespace ABI::Windows::UI::ViewManagement;
using namespace ABI::Windows::UI::Input;
using namespace ABI::Windows::Devices::Input;
using namespace ABI::Windows::System;
using namespace ABI::Windows::Foundation;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;

namespace mozilla {
namespace widget {
namespace winrt {

// statics
bool FrameworkView::sKeyboardIsVisible = false;
Rect FrameworkView::sKeyboardRect;
nsTArray<nsString>* sSettingsArray;

FrameworkView::FrameworkView(MetroApp* aMetroApp) :
  mDPI(-1.0f),
  mWidget(nullptr),
  mShuttingDown(false),
  mMetroApp(aMetroApp),
  mWindow(nullptr),
  mMetroInput(nullptr),
  mWinVisible(false),
  mWinActiveState(false)
{
  memset(&sKeyboardRect, 0, sizeof(Rect));
  sSettingsArray = new nsTArray<nsString>();
  LogFunction();
}

////////////////////////////////////////////////////
// IFrameworkView impl.

HRESULT
FrameworkView::Initialize(ICoreApplicationView* aAppView)
{
  LogFunction();
  if (mShuttingDown)
    return E_FAIL;

  aAppView->add_Activated(Callback<__FITypedEventHandler_2_Windows__CApplicationModel__CCore__CCoreApplicationView_Windows__CApplicationModel__CActivation__CIActivatedEventArgs_t>(
    this, &FrameworkView::OnActivated).Get(), &mActivated);

  //CoCreateInstance(CLSID_WICImagingFactory, nullptr,
  //                 CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mWicFactory));
  return S_OK;
}

HRESULT
FrameworkView::Uninitialize()
{
  return S_OK;
}

HRESULT
FrameworkView::Load(HSTRING aEntryPoint)
{
  return S_OK;
}

// called by winrt on startup
HRESULT
FrameworkView::Run()
{
  LogFunction();

  // Initialize XPCOM, create mWidget and go! We get a
  // callback in MetroAppShell::Run, in which we kick
  // off normal browser execution / event dispatching.
  mMetroApp->Run();

  // Gecko is completely shut down at this point.
  Log("Exiting FrameworkView::Run()");

  return S_OK;
}

HRESULT
FrameworkView::ActivateView()
{
  LogFunction();

  UpdateWidgetSizeAndPosition();
  MetroUtils::GetViewState(mViewState);

  nsIntRegion region(nsIntRect(0, 0, mWindowBounds.width, mWindowBounds.height));
  mWidget->Paint(region);

  // Activate the window, this kills the splash screen
  mWindow->Activate();

  ProcessLaunchArguments();
  AddEventHandlers();
  SetupContracts();

  return S_OK;
}

HRESULT
FrameworkView::SetWindow(ICoreWindow* aWindow)
{
  LogFunction();

  NS_ASSERTION(!mWindow, "Attempting to set a window on a view that already has a window!");
  NS_ASSERTION(aWindow, "Attempting to set a null window on a view!");

  mWindow = aWindow;
  UpdateLogicalDPI();
  return S_OK;
}

////////////////////////////////////////////////////
// FrameworkView impl.

void
FrameworkView::AddEventHandlers() {
  NS_ASSERTION(mWindow, "SetWindow must be called before AddEventHandlers!");
  NS_ASSERTION(mWidget, "SetWidget must be called before AddEventHAndlers!");

  mMetroInput = Make<MetroInput>(mWidget.Get(),
                                 mWindow.Get());

  mWindow->add_VisibilityChanged(Callback<__FITypedEventHandler_2_Windows__CUI__CCore__CCoreWindow_Windows__CUI__CCore__CVisibilityChangedEventArgs>(
    this, &FrameworkView::OnWindowVisibilityChanged).Get(), &mWindowVisibilityChanged);
  mWindow->add_Activated(Callback<__FITypedEventHandler_2_Windows__CUI__CCore__CCoreWindow_Windows__CUI__CCore__CWindowActivatedEventArgs_t>(
    this, &FrameworkView::OnWindowActivated).Get(), &mWindowActivated);
  mWindow->add_Closed(Callback<__FITypedEventHandler_2_Windows__CUI__CCore__CCoreWindow_Windows__CUI__CCore__CCoreWindowEventArgs_t>(
    this, &FrameworkView::OnWindowClosed).Get(), &mWindowClosed);
  mWindow->add_SizeChanged(Callback<__FITypedEventHandler_2_Windows__CUI__CCore__CCoreWindow_Windows__CUI__CCore__CWindowSizeChangedEventArgs_t>(
    this, &FrameworkView::OnWindowSizeChanged).Get(), &mWindowSizeChanged);

  mWindow->add_AutomationProviderRequested(Callback<__FITypedEventHandler_2_Windows__CUI__CCore__CCoreWindow_Windows__CUI__CCore__CAutomationProviderRequestedEventArgs_t>(
    this, &FrameworkView::OnAutomationProviderRequested).Get(), &mAutomationProviderRequested);

  HRESULT hr;
  ComPtr<ABI::Windows::Graphics::Display::IDisplayPropertiesStatics> dispProps;
  if (SUCCEEDED(GetActivationFactory(HStringReference(RuntimeClass_Windows_Graphics_Display_DisplayProperties).Get(), dispProps.GetAddressOf()))) {
    hr = dispProps->add_LogicalDpiChanged(Callback<ABI::Windows::Graphics::Display::IDisplayPropertiesEventHandler, FrameworkView>(
      this, &FrameworkView::OnLogicalDpiChanged).Get(), &mDisplayPropertiesChanged);
    LogHRESULT(hr);
  }

  ComPtr<ABI::Windows::UI::ViewManagement::IInputPaneStatics> inputStatic;
  if (SUCCEEDED(hr = GetActivationFactory(HStringReference(RuntimeClass_Windows_UI_ViewManagement_InputPane).Get(), inputStatic.GetAddressOf()))) {
    ComPtr<ABI::Windows::UI::ViewManagement::IInputPane> inputPane;
    if (SUCCEEDED(inputStatic->GetForCurrentView(inputPane.GetAddressOf()))) {
      inputPane->add_Hiding(Callback<__FITypedEventHandler_2_Windows__CUI__CViewManagement__CInputPane_Windows__CUI__CViewManagement__CInputPaneVisibilityEventArgs_t>(
        this, &FrameworkView::OnSoftkeyboardHidden).Get(), &mSoftKeyboardHidden);
      inputPane->add_Showing(Callback<__FITypedEventHandler_2_Windows__CUI__CViewManagement__CInputPane_Windows__CUI__CViewManagement__CInputPaneVisibilityEventArgs_t>(
        this, &FrameworkView::OnSoftkeyboardShown).Get(), &mSoftKeyboardShown);
    }
  }
}

// Called by MetroApp
void
FrameworkView::ShutdownXPCOM()
{
  LogFunction();
  mShuttingDown = true;

  if (mAutomationProvider) {
    ComPtr<IUIABridge> provider;
    mAutomationProvider.As(&provider);
    if (provider) {
      provider->Disconnect();
    }
  }
  mAutomationProvider = nullptr;

  mMetroInput = nullptr;
  delete sSettingsArray;
  sSettingsArray = nullptr;
  mWidget = nullptr;
  mMetroApp = nullptr;
  mWindow = nullptr;
}

void
FrameworkView::SetCursor(CoreCursorType aCursorType, DWORD aCustomId)
{
  if (mShuttingDown) {
    return;
  }
  NS_ASSERTION(mWindow, "SetWindow must be called before SetCursor!");
  ComPtr<ABI::Windows::UI::Core::ICoreCursorFactory> factory;
  AssertHRESULT(GetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Core_CoreCursor).Get(), factory.GetAddressOf()));
  ComPtr<ABI::Windows::UI::Core::ICoreCursor> cursor;
  AssertHRESULT(factory->CreateCursor(aCursorType, aCustomId, cursor.GetAddressOf()));
  mWindow->put_PointerCursor(cursor.Get());
}

void
FrameworkView::ClearCursor()
{
  if (mShuttingDown) {
    return;
  }
  NS_ASSERTION(mWindow, "SetWindow must be called before ClearCursor!");
  mWindow->put_PointerCursor(nullptr);
}

void
FrameworkView::UpdateLogicalDPI()
{
  ComPtr<ABI::Windows::Graphics::Display::IDisplayPropertiesStatics> dispProps;
  HRESULT hr = GetActivationFactory(HStringReference(RuntimeClass_Windows_Graphics_Display_DisplayProperties).Get(),
                                    dispProps.GetAddressOf());
  AssertHRESULT(hr);
  FLOAT value;
  AssertHRESULT(dispProps->get_LogicalDpi(&value));
  SetDpi(value);
}

void
FrameworkView::GetBounds(nsIntRect &aRect)
{
  // May be called by compositor thread
  if (mShuttingDown) {
    return;
  }
  aRect = mWindowBounds;
}

void
FrameworkView::UpdateWidgetSizeAndPosition()
{
  if (mShuttingDown)
    return;

  NS_ASSERTION(mWindow, "SetWindow must be called before UpdateWidgetSizeAndPosition!");
  NS_ASSERTION(mWidget, "SetWidget must be called before UpdateWidgetSizeAndPosition!");

  mWidget->Move(0, 0);
  mWidget->Resize(0, 0, mWindowBounds.width, mWindowBounds.height, true);
  mWidget->SizeModeChanged();
}

bool
FrameworkView::IsEnabled() const
{
  return mWinActiveState;
}

bool
FrameworkView::IsVisible() const
{
  // we could check the wnd in MetroWidget for this, but
  // generally we don't let nsIWidget control visibility
  // or activation.
  return mWinVisible;
}

void FrameworkView::SetDpi(float aDpi)
{
  if (aDpi != mDPI) {
    LogFunction();

    mDPI = aDpi;
    // Often a DPI change implies a window size change.
    NS_ASSERTION(mWindow, "SetWindow must be called before SetDpi!");
    Rect logicalBounds;
    mWindow->get_Bounds(&logicalBounds);

    // convert to physical (device) pixels
    mWindowBounds = MetroUtils::LogToPhys(logicalBounds);

    // notify the widget that dpi has changed
    if (mWidget) {
      mWidget->ChangedDPI();
    }
  }
}

void
FrameworkView::SetWidget(MetroWidget* aWidget)
{
  NS_ASSERTION(!mWidget, "Attempting to set a widget for a view that already has a widget!");
  NS_ASSERTION(aWidget, "Attempting to set a null widget for a view!");
  LogFunction();
  mWidget = aWidget;
  mWidget->FindMetroWindow();
}

////////////////////////////////////////////////////
// Event handlers

void
FrameworkView::SendActivationEvent() 
{
  if (mShuttingDown) {
    return;
  }
  NS_ASSERTION(mWindow, "SetWindow must be called before SendActivationEvent!");
  mWidget->Activated(mWinActiveState);
  UpdateWidgetSizeAndPosition();
  EnsureAutomationProviderCreated();
}

HRESULT
FrameworkView::OnWindowVisibilityChanged(ICoreWindow* aWindow,
                                         IVisibilityChangedEventArgs* aArgs)
{
  // If we're visible, or we can't determine if we're visible, just store
  // that we are visible.
  boolean visible;
  mWinVisible = FAILED(aArgs->get_Visible(&visible)) || visible;
  return S_OK;
}

HRESULT
FrameworkView::OnActivated(ICoreApplicationView* aApplicationView,
                           IActivatedEventArgs* aArgs)
{
  LogFunction();

  ApplicationExecutionState state;
  aArgs->get_PreviousExecutionState(&state);
  bool startup = state == ApplicationExecutionState::ApplicationExecutionState_Terminated ||
                 state == ApplicationExecutionState::ApplicationExecutionState_ClosedByUser ||
                 state == ApplicationExecutionState::ApplicationExecutionState_NotRunning;
  ProcessActivationArgs(aArgs, startup);
  return S_OK;
}

HRESULT
FrameworkView::OnSoftkeyboardHidden(IInputPane* aSender,
                                    IInputPaneVisibilityEventArgs* aArgs)
{
  LogFunction();
  if (mShuttingDown)
    return S_OK;
  sKeyboardIsVisible = false;
  memset(&sKeyboardRect, 0, sizeof(Rect));
  MetroUtils::FireObserver("metro_softkeyboard_hidden");
  aArgs->put_EnsuredFocusedElementInView(true);
  return S_OK;
}

HRESULT
FrameworkView::OnSoftkeyboardShown(IInputPane* aSender,
                                   IInputPaneVisibilityEventArgs* aArgs)
{
  LogFunction();
  if (mShuttingDown)
    return S_OK;
  sKeyboardIsVisible = true;
  aSender->get_OccludedRect(&sKeyboardRect);
  MetroUtils::FireObserver("metro_softkeyboard_shown");
  aArgs->put_EnsuredFocusedElementInView(true);
  return S_OK;
}

HRESULT
FrameworkView::OnWindowClosed(ICoreWindow* aSender, ICoreWindowEventArgs* aArgs)
{
  // this doesn't seem very reliable
  return S_OK;
}

void
FrameworkView::FireViewStateObservers()
{
  ApplicationViewState state;
  MetroUtils::GetViewState(state);
  if (state == mViewState) {
    return;
  }
  mViewState = state;
  nsAutoString name;
  switch (mViewState) {
    case ApplicationViewState_FullScreenLandscape:
      name.AssignLiteral("landscape");
    break;
    case ApplicationViewState_Filled:
      name.AssignLiteral("filled");
    break;
    case ApplicationViewState_Snapped:
      name.AssignLiteral("snapped");
    break;
    case ApplicationViewState_FullScreenPortrait:
      name.AssignLiteral("portrait");
    break;
    default:
      NS_WARNING("Unknown view state");
    return;
  };
  MetroUtils::FireObserver("metro_viewstate_changed", name.get());
}

HRESULT
FrameworkView::OnWindowSizeChanged(ICoreWindow* aSender, IWindowSizeChangedEventArgs* aArgs)
{
  LogFunction();

  if (mShuttingDown) {
    return S_OK;
  }

  NS_ASSERTION(mWindow, "SetWindow must be called before OnWindowSizeChanged!");
  Rect logicalBounds;
  mWindow->get_Bounds(&logicalBounds);
  mWindowBounds = MetroUtils::LogToPhys(logicalBounds);

  UpdateWidgetSizeAndPosition();
  FireViewStateObservers();
  return S_OK;
}

HRESULT
FrameworkView::OnWindowActivated(ICoreWindow* aSender, IWindowActivatedEventArgs* aArgs)
{
  LogFunction();
  if (mShuttingDown || !mWidget)
    return E_FAIL;
  CoreWindowActivationState state;
  aArgs->get_WindowActivationState(&state);
  mWinActiveState = !(state == CoreWindowActivationState::CoreWindowActivationState_Deactivated);
  SendActivationEvent();
  return S_OK;
}

HRESULT
FrameworkView::OnLogicalDpiChanged(IInspectable* aSender)
{
  LogFunction();
  UpdateLogicalDPI();
  if (mWidget) {
    mWidget->Invalidate();
  }
  return S_OK;
}

bool
FrameworkView::EnsureAutomationProviderCreated()
{
#ifdef ACCESSIBILITY
  if (!mWidget || mShuttingDown)
    return false;

  if (mAutomationProvider) {
    return true;
  }

  Accessible *rootAccessible = mWidget->GetRootAccessible();
  if (rootAccessible) {
    IInspectable* inspectable;
    HRESULT hr;
    AssertRetHRESULT(hr = UIABridge_CreateInstance(&inspectable), hr); // Addref
    IUIABridge* bridge = nullptr;
    inspectable->QueryInterface(IID_IUIABridge, (void**)&bridge); // Addref
    if (bridge) {
      bridge->Init(this, mWindow.Get(), (ULONG)rootAccessible);
      mAutomationProvider = inspectable;
      inspectable->Release();
      return true;
    }
  }
#endif
  return false;
}

HRESULT
FrameworkView::OnAutomationProviderRequested(ICoreWindow* aSender,
                                             IAutomationProviderRequestedEventArgs* aArgs)
{
  LogFunction();
  if (!EnsureAutomationProviderCreated())
    return E_FAIL;
  Log("OnAutomationProviderRequested %X", mAutomationProvider.Get());
  HRESULT hr = aArgs->put_AutomationProvider(mAutomationProvider.Get());
  if (FAILED(hr)) {
    Log("put failed? %X", hr);
  }
  return S_OK;
}

} } }
